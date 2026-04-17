#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    pthread_mutex_t *first;
    pthread_mutex_t *second;
    int              hold_ms;
} locker_arg_t;

static void *locker_fn(void *arg) {
    locker_arg_t *a = (locker_arg_t *)arg;

    pg_logf(NULL, "lock first  (%p)", (void *)a->first);
    pthread_mutex_lock(a->first);
    pg_logf(NULL, "  got first; sleeping %d ms before grabbing second", a->hold_ms);

    struct timespec ts = { .tv_nsec = (long)a->hold_ms * 1000000L };
    nanosleep(&ts, NULL);

    pg_logf(NULL, "lock second (%p) -- this is where the deadlock bites", (void *)a->second);
    pthread_mutex_lock(a->second);   /* hangs here in deadlock */

    pg_logf(NULL, "  got both; releasing");
    pthread_mutex_unlock(a->second);
    pthread_mutex_unlock(a->first);
    return NULL;
}

static int dl_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;

    pg_phase(ctx, "two threads, opposite lock order");
    pg_logf (ctx, "T1 wants A then B; T2 wants B then A; small hold delay biases the race");
    pg_logf (ctx, "watchdog will fire after 2s if both threads end up waiting forever");
    pg_expect(ctx, "watchdog_fired", 1);

    chaos_thread_watchdog(2ULL * 1000000000ULL, "classic deadlock");

    locker_arg_t arg1 = { .first = &a, .second = &b, .hold_ms = 50 };
    locker_arg_t arg2 = { .first = &b, .second = &a, .hold_ms = 50 };

    chaos_thread_t *t1 = chaos_thread_spawn(locker_fn, &arg1, "T1");
    chaos_thread_t *t2 = chaos_thread_spawn(locker_fn, &arg2, "T2");

    /* These joins never return — the watchdog terminates the process. */
    chaos_thread_join(t1);
    chaos_thread_join(t2);

    /* Unreachable in practice, but be tidy if a future change makes the */
    /* deadlock probabilistic and the threads occasionally escape.       */
    pg_actual(ctx, "watchdog_fired", 0);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "deadlock_classic",
    .title       = "Deadlock: opposite lock order",
    .one_liner   = "two threads, two mutexes, AB vs BA",
    .description = "Two threads acquire two mutexes in opposite orders; each waits forever for the other. Watchdog cuts the hang.",
    .expected    = "watchdog fires; PG_EV_FAULT emitted; process exits 2",
    .lesson      =
        "Why this hangs: T1 holds A and waits for B. T2 holds B and waits\n"
        "for A. Each thread is waiting for what the other holds. Neither\n"
        "ever releases. The watchdog cuts the hang.\n"
        "\n"
        "Fixes:\n"
        "  - Always acquire locks in a fixed global order. If every thread\n"
        "    acquires A before B, no cycle is possible.\n"
        "  - pthread_mutex_trylock + backoff. If you can't get B, release\n"
        "    A and retry.\n"
        "  - Use a single mutex (eliminate the locking dance entirely).\n"
        "  - lockdep-style runtime ordering checks in dev builds.",
    .category    = PG_CAT_CONCURRENCY,
    .run         = dl_run,
};

PG_SCENARIO_REGISTER(scen);
