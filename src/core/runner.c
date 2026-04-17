#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/runner.h"
#include "playground/chaos_io.h"   /* chaos_clock_seed */
#include "rand.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct pg_runctx {
    int        event_fd_w;
    uint64_t   seed;
    pg_xosh_t  rng;
    uint32_t   thread_tag;
    uint64_t   start_ns;       /* monotonic clock at scenario start */
};

uint64_t pg_runctx_seed(const pg_runctx_t *ctx) {
    return ctx ? ctx->seed : 0;
}

uint64_t pg_runctx_rand(pg_runctx_t *ctx) {
    return ctx ? pg_xosh_next(&ctx->rng) : 0;
}

/* The active scenario context inside the child process. Set once before
 * the scenario runs, then read by emit helpers from any thread. */
static pg_runctx_t *g_runctx = NULL;

/* Per-thread tag used to attribute events to the scenario thread that
 * emitted them. Set by chaos_thread_spawn's wrapper; 0 for unwrapped threads. */
__thread uint32_t g_thread_tag = 0;

static _Atomic uint32_t g_next_thread_tag = 1;

uint32_t pg_alloc_thread_tag(void) {
    return atomic_fetch_add(&g_next_thread_tag, 1);
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

void pg_emit(pg_runctx_t *ctx, const pg_event_t *ev) {
    if (!ctx) ctx = g_runctx;
    if (!ctx || ctx->event_fd_w < 0 || !ev) return;
    pg_event_t copy = *ev;
    copy.magic   = PG_EVENT_MAGIC;
    copy.version = PG_EVENT_VERSION;
    if (copy.ts_ns == 0) copy.ts_ns = now_ns() - ctx->start_ns;
    if (copy.thread_tag == 0) {
        copy.thread_tag = g_thread_tag ? g_thread_tag : ctx->thread_tag;
    }
    /* Loop on partial writes / EINTR. POSIX guarantees writes <= PIPE_BUF
     * (>= 512 on macOS/Linux) are atomic on pipes vs. other writers, so the
     * record itself never interleaves with another thread's emit; the loop
     * just handles signal interruption and writes that the kernel splits. */
    const uint8_t *p   = (const uint8_t *)&copy;
    size_t         off = 0;
    while (off < sizeof(copy)) {
        ssize_t w = write(ctx->event_fd_w, p + off, sizeof(copy) - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;     /* pipe broken; parent has gone */
        }
        off += (size_t)w;
    }
}

void pg_logf(pg_runctx_t *ctx, const char *fmt, ...) {
    pg_event_t ev = { .kind = PG_EV_LOG };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);
    pg_emit(ctx, &ev);
}

void pg_phase(pg_runctx_t *ctx, const char *name) {
    pg_event_t ev = { .kind = PG_EV_PHASE };
    snprintf(ev.text, sizeof(ev.text), "%s", name ? name : "");
    pg_emit(ctx, &ev);
}

static void emit_kv(pg_runctx_t *ctx, pg_event_kind_t k, const char *key, int64_t v) {
    pg_event_t ev = { .kind = (uint32_t)k, .num = v };
    snprintf(ev.key, sizeof(ev.key), "%s", key ? key : "");
    pg_emit(ctx, &ev);
}

void pg_count (pg_runctx_t *c, const char *k, int64_t v) { emit_kv(c, PG_EV_COUNTER, k, v); }
void pg_gauge (pg_runctx_t *c, const char *k, int64_t v) { emit_kv(c, PG_EV_GAUGE,   k, v); }
void pg_expect(pg_runctx_t *c, const char *k, int64_t v) { emit_kv(c, PG_EV_EXPECT,  k, v); }
void pg_actual(pg_runctx_t *c, const char *k, int64_t v) { emit_kv(c, PG_EV_ACTUAL,  k, v); }

void pg_sut_fault(pg_runctx_t *ctx, const char *what) {
    pg_event_t ev = { .kind = PG_EV_SUT_FAULT };
    snprintf(ev.text, sizeof(ev.text), "%s", what ? what : "");
    pg_emit(ctx, &ev);
}

void pg_watchdog(pg_runctx_t *ctx, const char *what) {
    pg_event_t ev = { .kind = PG_EV_WATCHDOG };
    snprintf(ev.text, sizeof(ev.text), "%s", what ? what : "");
    pg_emit(ctx, &ev);
}

void pg_done(pg_runctx_t *ctx) {
    pg_event_t ev = { .kind = PG_EV_DONE };
    pg_emit(ctx, &ev);
}

const char *pg_event_kind_name(pg_event_kind_t k) {
    switch (k) {
    case PG_EV_LOG:         return "log";
    case PG_EV_COUNTER:     return "counter";
    case PG_EV_GAUGE:       return "gauge";
    case PG_EV_PHASE:       return "phase";
    case PG_EV_EXPECT:      return "expect";
    case PG_EV_ACTUAL:      return "actual";
    case PG_EV_SUT_FAULT:   return "sut_fault";
    case PG_EV_DONE:        return "done";
    case PG_EV_CHILD_CRASH: return "child_crash";
    case PG_EV_WATCHDOG:    return "watchdog";
    }
    return "?";
}

const char *pg_category_name(pg_category_t cat) {
    switch (cat) {
    case PG_CAT_CONCURRENCY: return "concurrency";
    case PG_CAT_MEMORY:      return "memory";
    case PG_CAT_NETWORK:     return "network";
    case PG_CAT_TIMEIO:      return "timeio";
    case PG_CAT__COUNT:      break;
    }
    return "?";
}

/* ------------- async-signal-safe fault handler in child -------------- */

static int g_child_event_fd = -1;

static void child_fault_handler(int sig) {
    pg_event_t ev = { 0 };
    /* Hand-roll header fields for async-signal safety; pg_emit isn't called. */
    ev.magic   = PG_EVENT_MAGIC;
    ev.version = PG_EVENT_VERSION;
    ev.kind    = PG_EV_CHILD_CRASH;
    const char *prefix = "child crashed: ";
    const char *name   = "signal";
    switch (sig) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGBUS:  name = "SIGBUS";  break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGFPE:  name = "SIGFPE";  break;
    case SIGILL:  name = "SIGILL";  break;
    }
    /* Hand-roll the string copy to stay async-signal-safe. */
    size_t i = 0;
    for (const char *p = prefix; *p && i < sizeof(ev.text) - 1; ++p) ev.text[i++] = *p;
    for (const char *p = name;   *p && i < sizeof(ev.text) - 1; ++p) ev.text[i++] = *p;
    ev.text[i] = '\0';

    if (g_child_event_fd >= 0) {
        /* Best-effort single write; if it short-writes, the parent's resync
         * will skip the byte stream until the next 256B boundary. */
        ssize_t w = write(g_child_event_fd, &ev, sizeof(ev));
        (void)w;
    }
    /* SA_RESETHAND already restored SIG_DFL on entry, so raising re-enters
     * the default handler and terminates with the right exit status. */
    raise(sig);
}

static void install_child_handlers(int event_fd) {
    g_child_event_fd = event_fd;
    int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = child_fault_handler;
        sa.sa_flags   = SA_RESETHAND;
        sigaction(sigs[i], &sa, NULL);
    }
}

/* -------------------------- runner API -------------------------- */

int pg_run_start(const pg_scenario_t *s, uint64_t seed, pg_run_handle_t *out) {
    if (!s || !out) {
        errno = EINVAL;
        return -1;
    }

    int evpipe[2];
    if (pipe(evpipe) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(evpipe[0]);
        close(evpipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* ------- child ------- */
        close(evpipe[0]);
        install_child_handlers(evpipe[1]);

        pg_runctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.event_fd_w = evpipe[1];
        ctx.seed       = seed;
        ctx.thread_tag = 0;
        ctx.start_ns   = now_ns();
        pg_xosh_seed(&ctx.rng, seed);
        /* Seed the process-global clock RNG so jitter is reproducible across
         * runs with the same scenario seed. Distinct mask so it doesn't
         * collide with the scenario's own RNG stream. */
        chaos_clock_seed(seed ^ 0xC10CC10CC10CC10CULL);
        g_runctx = &ctx;

        /* Resource caps so a runaway scenario can't take down the box.
         * Configurable via env so time/IO scenarios with intentionally long
         * waits can be granted more headroom. */
        const char *limit_env = getenv("PG_CPU_LIMIT_SECONDS");
        rlim_t      cpu_limit = 30;
        if (limit_env && *limit_env) {
            unsigned long v = strtoul(limit_env, NULL, 10);
            if (v > 0) cpu_limit = (rlim_t)v;
        }
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = cpu_limit;
        setrlimit(RLIMIT_CPU, &rl);

        void *state = NULL;
        int   rc    = 0;

        if (s->init) rc = s->init(&ctx, &state);
        if (rc == 0) rc = s->run (&ctx, state);
        if (s->teardown) s->teardown(&ctx, state);

        pg_done(&ctx);
        close(evpipe[1]);
        _exit(rc == 0 ? 0 : 1);
    }

    /* ------- parent ------- */
    close(evpipe[1]);
    out->child_pid = pid;
    out->event_fd  = evpipe[0];
    out->ctrl_fd   = -1;
    out->seed      = seed;
    return 0;
}

int pg_run_poll(pg_run_handle_t *h, pg_event_t *ev_out, int timeout_ms) {
    if (!h || h->event_fd < 0 || !ev_out) return -1;

    if (timeout_ms >= 0) {
        struct pollfd pf = { .fd = h->event_fd, .events = POLLIN };
        int pr = poll(&pf, 1, timeout_ms);
        if (pr == 0) return -2;       /* timeout */
        if (pr  < 0) return -1;
    }

    /* Read exactly one record. Loop on partial reads to recover from
     * interrupted syscalls; bail out cleanly on EOF. */
    size_t got = 0;
    char  *buf = (char *)ev_out;
    while (got < sizeof(*ev_out)) {
        ssize_t n = read(h->event_fd, buf + got, sizeof(*ev_out) - got);
        if (n == 0) {
            return got == 0 ? 0 : -1;   /* EOF; partial = corruption */
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    /* Validate magic + version. If a child crashed mid-write, the next
     * record may be misaligned garbage; this catches it loudly instead
     * of silently decoding wrong fields. */
    if (ev_out->magic != PG_EVENT_MAGIC || ev_out->version != PG_EVENT_VERSION) {
        fprintf(stderr,
            "pg_run_poll: bad event header (magic=0x%04x version=%u expected 0x%04x v%u)\n",
            (unsigned)ev_out->magic, (unsigned)ev_out->version,
            (unsigned)PG_EVENT_MAGIC, (unsigned)PG_EVENT_VERSION);
        return -1;
    }
    return 1;
}

void pg_run_abort(pg_run_handle_t *h) {
    if (!h || h->child_pid <= 0) return;
    kill(h->child_pid, SIGTERM);
    /* not waiting here; reap will collect */
}

int pg_run_reap(pg_run_handle_t *h, int *exit_status) {
    if (!h) return -1;
    int status = 0;
    if (h->child_pid > 0) {
        pid_t r;
        do { r = waitpid(h->child_pid, &status, 0); }
        while (r < 0 && errno == EINTR);
        if (r < 0) return -1;
    }
    if (exit_status) *exit_status = status;
    if (h->event_fd >= 0) { close(h->event_fd); h->event_fd = -1; }
    h->child_pid = -1;
    return 0;
}
