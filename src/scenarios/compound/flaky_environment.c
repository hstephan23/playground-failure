/* Flaky environment: jittery net + slow fsync + clock noise.
 *
 * A naïve "every 100ms" heartbeat loop under realistic production noise.
 * Each layer of variance is small; together they make the actual cadence
 * wildly different from the configured one. */

#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_net.h"
#include "playground/chaos_io.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    TICKS          = 20,
    TARGET_MS      = 100,
    NET_DELAY_MS   = 3,
    FSYNC_DELAY_MS = 15,
    JITTER_MS      = 2,
};

/* Drain proxy_b in a background thread so sender writes don't stall. */
static void *drain_fn(void *arg) {
    int fd = *(int *)arg;
    char buf[512];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
    }
    return NULL;
}

static int run(pg_runctx_t *ctx, void *state) {
    (void)state;

    chaos_net_knobs_t net_k = {
        .delay_min_ns    = (uint64_t)(NET_DELAY_MS - 1) * 1000000ULL,
        .delay_max_ns    = (uint64_t)(NET_DELAY_MS + 2) * 1000000ULL,
        .max_chunk_bytes = 32,
    };
    chaos_pair_t pair;
    if (chaos_net_pair(&pair, &net_k, pg_runctx_seed(ctx)) < 0) {
        pg_sut_fault(ctx, "chaos_net_pair failed");
        return 1;
    }

    int io_fd = chaos_io_open("/tmp/heartbeat");
    if (io_fd < 0) {
        pg_sut_fault(ctx, "chaos_io_open failed");
        chaos_net_close(&pair);
        return 1;
    }
    chaos_io_set_fsync_delay(io_fd, (uint64_t)FSYNC_DELAY_MS * 1000000ULL);
    chaos_clock_jitter_set((uint64_t)JITTER_MS * 1000000ULL);

    /* drainer so net writes keep flowing */
    pthread_t drain_th;
    pthread_create(&drain_th, NULL, drain_fn, &pair.fd_b);

    pg_phase (ctx, "20 'heartbeats' supposed to fire every 100ms");
    pg_logf  (ctx, "each tick: net write (~3ms delay) + fsync (~15ms) + sleep to deadline");
    pg_expect(ctx, "max_tick_ms", TARGET_MS);

    int64_t min_ms = (int64_t)1 << 62;
    int64_t max_ms = 0;
    int64_t sum_ms = 0;
    uint64_t start = chaos_clock_now_ns();
    uint64_t prev  = start;

    for (int i = 1; i <= TICKS; ++i) {
        /* "work" */
        char tok = (char)('A' + (i % 26));
        write(pair.fd_a, &tok, 1);

        char payload[64]; memset(payload, tok, sizeof(payload));
        chaos_io_write(io_fd, payload, sizeof(payload));
        chaos_io_fsync(io_fd);

        /* sleep until the next deadline — classic cadence loop */
        uint64_t deadline = start + (uint64_t)i * TARGET_MS * 1000000ULL;
        for (;;) {
            uint64_t now = chaos_clock_now_ns();
            if (now >= deadline) break;
            uint64_t remaining = deadline - now;
            struct timespec ts = {
                .tv_sec  = (time_t)(remaining / 1000000000ULL),
                .tv_nsec = (long)  (remaining % 1000000000ULL),
            };
            nanosleep(&ts, NULL);
        }

        uint64_t now = chaos_clock_now_ns();
        int64_t  dt  = (int64_t)((now - prev) / 1000000);
        prev = now;
        if (dt < min_ms) min_ms = dt;
        if (dt > max_ms) max_ms = dt;
        sum_ms += dt;
    }

    pg_actual(ctx, "max_tick_ms", max_ms);
    pg_gauge (ctx, "min_tick_ms", min_ms);
    pg_gauge (ctx, "max_tick_ms", max_ms);
    pg_gauge (ctx, "mean_tick_ms", sum_ms / TICKS);
    pg_logf  (ctx, "target=%d ms  min=%lld  mean=%lld  max=%lld",
              TARGET_MS, (long long)min_ms,
              (long long)(sum_ms / TICKS), (long long)max_ms);
    pg_logf  (ctx, "naive heartbeat code assumes jitter <= budget; in prod the outliers will timeout you.");

    chaos_clock_jitter_set(0);
    chaos_io_close(io_fd);
    chaos_net_close(&pair);
    pthread_join(drain_th, NULL);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "flaky_environment",
    .title       = "Flaky env: jitter everywhere",
    .one_liner   = "100ms heartbeat under net + fsync + clock noise",
    .description = "Measures how much a 100ms cadence slips when every layer adds a little variance. "
                   "Individually each layer is tolerable; combined they blow the budget.",
    .expected    = "max_tick_ms >> target (100); mean close to target",
    .lesson      =
        "Why 'it worked in dev' fails in prod: every single layer adds\n"
        "variance. Network jitter. Disk tail latency. Scheduler preempts.\n"
        "NTP adjusts the clock. Each layer is ~2-15 ms in isolation; each\n"
        "is forgivable. Compounded across a single iteration, the nominal\n"
        "100 ms budget is regularly violated.\n"
        "\n"
        "Naive heartbeat timeouts (e.g. 'declare peer dead if no beat for\n"
        "200 ms') fire false positives under load.\n"
        "\n"
        "Fixes:\n"
        "  - Timeout = 3-5× the nominal interval, not 2×.\n"
        "  - Use CLOCK_MONOTONIC for elapsed math (never REALTIME).\n"
        "  - Measure p99 in prod, not just p50.\n"
        "  - Inject synthetic jitter in tests so the chaos is familiar.",
    .category    = PG_CAT_COMPOUND,
    .run         = run,
};

PG_SCENARIO_REGISTER(scen);
