#include "playground/chaos_thread.h"
#include "playground/event.h"
#include "playground/scenario.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* From runner.c */
extern __thread uint32_t g_thread_tag;
extern uint32_t          pg_alloc_thread_tag(void);

struct chaos_thread {
    pthread_t pt;
    char      tag_name[32];
    uint32_t  tag_id;
    int       joined;
};

typedef struct {
    void *(*user_fn)(void *);
    void  *user_arg;
    uint32_t tag_id;
} wrap_arg_t;

static void *thread_wrap(void *arg) {
    wrap_arg_t *w = (wrap_arg_t *)arg;
    g_thread_tag = w->tag_id;
    void *(*fn)(void *) = w->user_fn;
    void *ua = w->user_arg;
    free(w);
    return fn(ua);
}

chaos_thread_t *chaos_thread_spawn(void *(*fn)(void *), void *arg, const char *tag) {
    if (!fn) return NULL;
    chaos_thread_t *t = (chaos_thread_t *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    snprintf(t->tag_name, sizeof(t->tag_name), "%s", tag ? tag : "");
    t->tag_id = pg_alloc_thread_tag();

    wrap_arg_t *w = (wrap_arg_t *)malloc(sizeof(*w));
    if (!w) { free(t); return NULL; }
    w->user_fn  = fn;
    w->user_arg = arg;
    w->tag_id   = t->tag_id;

    if (pthread_create(&t->pt, NULL, thread_wrap, w) != 0) {
        free(w);
        free(t);
        return NULL;
    }
    return t;
}

int chaos_thread_join(chaos_thread_t *t) {
    if (!t) return -1;
    int rc = pthread_join(t->pt, NULL);
    t->joined = 1;
    free(t);
    return rc == 0 ? 0 : -1;
}

int chaos_thread_cancel(chaos_thread_t *t) {
    if (!t) return -1;
    return pthread_cancel(t->pt) == 0 ? 0 : -1;
}

typedef struct {
    pthread_t target;
    uint64_t  after_ns;
} cancel_arg_t;

static void *cancel_after_fn(void *arg) {
    cancel_arg_t *ca = (cancel_arg_t *)arg;
    struct timespec ts = {
        .tv_sec  = (time_t)(ca->after_ns / 1000000000ULL),
        .tv_nsec = (long)  (ca->after_ns % 1000000000ULL),
    };
    nanosleep(&ts, NULL);
    pthread_cancel(ca->target);
    free(ca);
    return NULL;
}

int chaos_thread_cancel_at_ns(chaos_thread_t *t, uint64_t after_ns) {
    if (!t) return -1;
    cancel_arg_t *ca = (cancel_arg_t *)malloc(sizeof(*ca));
    if (!ca) return -1;
    ca->target   = t->pt;
    ca->after_ns = after_ns;
    pthread_t cancel_th;
    if (pthread_create(&cancel_th, NULL, cancel_after_fn, ca) != 0) {
        free(ca);
        return -1;
    }
    pthread_detach(cancel_th);
    return 0;
}

int chaos_thread_inject_delay(pg_runctx_t *ctx, uint64_t min_ns, uint64_t max_ns) {
    uint64_t span  = max_ns > min_ns ? max_ns - min_ns : 0;
    uint64_t r     = pg_runctx_rand(ctx);
    uint64_t delay = min_ns + (span ? (r % span) : 0);
    struct timespec ts = {
        .tv_sec  = (time_t)(delay / 1000000000ULL),
        .tv_nsec = (long)  (delay % 1000000000ULL),
    };
    return nanosleep(&ts, NULL);
}

typedef struct {
    uint64_t timeout_ns;
    char     what[64];
} wd_arg_t;

static void *watchdog_fn(void *arg) {
    wd_arg_t *wd = (wd_arg_t *)arg;
    struct timespec ts = {
        .tv_sec  = (time_t)(wd->timeout_ns / 1000000000ULL),
        .tv_nsec = (long)  (wd->timeout_ns % 1000000000ULL),
    };
    nanosleep(&ts, NULL);

    char msg[128];
    snprintf(msg, sizeof(msg), "watchdog: %s", wd->what);
    pg_fault(NULL, msg);
    /* Hard exit — the scenario is hung; only way out is to terminate the child.
     * Parent sees PG_EV_FAULT then EOF, reaps exit status 2. */
    _exit(2);
}

void chaos_thread_watchdog(uint64_t timeout_ns, const char *what) {
    wd_arg_t *wd = (wd_arg_t *)malloc(sizeof(*wd));
    if (!wd) return;
    wd->timeout_ns = timeout_ns;
    snprintf(wd->what, sizeof(wd->what), "%s", what ? what : "timeout");

    pthread_t t;
    if (pthread_create(&t, NULL, watchdog_fn, wd) == 0) {
        pthread_detach(t);
    } else {
        free(wd);
    }
}
