/* Thundering herd: N threads wake at the same instant and race for one
 * mutex whose critical section is a slow fsync. Illustrates that even
 * "fast" per-request work collapses under contention + tail latency. */

#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_thread.h"
#include "playground/chaos_io.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum {
    WORKERS      = 8,
    ITERS        = 6,
    FSYNC_DELAY  = (int64_t)30 * 1000000,   /* 30 ms */
};

typedef struct {
    int              id;
    pg_runctx_t     *ctx;
    int              io_fd;
    pthread_mutex_t *mu;
    pthread_mutex_t *start_mu;
    pthread_cond_t  *start_cv;
    int             *started;   /* becomes 1 when main broadcasts */
    int64_t         *total_work_ns;
    int64_t         *total_wait_ns;
    pthread_mutex_t *tot_mu;
} worker_arg_t;

static void *worker_fn(void *arg) {
    worker_arg_t *a = (worker_arg_t *)arg;

    /* Wait for the starting gun */
    pthread_mutex_lock(a->start_mu);
    while (!*a->started) pthread_cond_wait(a->start_cv, a->start_mu);
    pthread_mutex_unlock(a->start_mu);

    char  payload[64]; memset(payload, '0' + (a->id % 10), sizeof(payload));
    for (int i = 0; i < ITERS; ++i) {
        uint64_t t_before = chaos_clock_now_ns();
        pthread_mutex_lock(a->mu);
        uint64_t t_acquired = chaos_clock_now_ns();

        /* critical section: slow fsync */
        chaos_io_write(a->io_fd, payload, sizeof(payload));
        chaos_io_fsync(a->io_fd);

        uint64_t t_done = chaos_clock_now_ns();
        pthread_mutex_unlock(a->mu);

        pthread_mutex_lock(a->tot_mu);
        *a->total_wait_ns += (int64_t)(t_acquired - t_before);
        *a->total_work_ns += (int64_t)(t_done - t_acquired);
        pthread_mutex_unlock(a->tot_mu);
    }
    return NULL;
}

static int run(pg_runctx_t *ctx, void *state) {
    (void)state;

    int io_fd = chaos_io_open("/tmp/herd");
    if (io_fd < 0) { pg_sut_fault(ctx, "chaos_io_open failed"); return 1; }
    chaos_io_set_fsync_delay(io_fd, FSYNC_DELAY);

    pthread_mutex_t mu       = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t start_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  start_cv = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t tot_mu   = PTHREAD_MUTEX_INITIALIZER;
    int             started  = 0;
    int64_t         total_wait = 0, total_work = 0;

    pg_phase (ctx, "8 workers, 6 iterations each, all racing for one mutex");
    pg_logf  (ctx, "critical section is a 30ms fsync — pure serial work");
    /* expected: total wall-clock ≈ WORKERS * ITERS * FSYNC_DELAY (fully serialized) */
    int64_t expected_ms = ((int64_t)WORKERS * ITERS * FSYNC_DELAY) / 1000000;
    pg_expect(ctx, "wall_ms_approx", expected_ms);

    chaos_thread_t *workers[WORKERS];
    worker_arg_t    args[WORKERS];
    for (int i = 0; i < WORKERS; ++i) {
        args[i] = (worker_arg_t){
            .id = i + 1, .ctx = ctx, .io_fd = io_fd,
            .mu = &mu, .start_mu = &start_mu, .start_cv = &start_cv,
            .started = &started,
            .total_wait_ns = &total_wait, .total_work_ns = &total_work,
            .tot_mu = &tot_mu,
        };
        workers[i] = chaos_thread_spawn(worker_fn, &args[i], "worker");
    }

    /* let them all block on the cond var */
    usleep(10000);

    uint64_t t0 = chaos_clock_now_ns();
    pthread_mutex_lock(&start_mu);
    started = 1;
    pthread_cond_broadcast(&start_cv);
    pthread_mutex_unlock(&start_mu);

    for (int i = 0; i < WORKERS; ++i) chaos_thread_join(workers[i]);
    uint64_t dt = chaos_clock_now_ns() - t0;
    int64_t  wall_ms = (int64_t)(dt / 1000000);

    int64_t total_ops    = (int64_t)WORKERS * ITERS;
    int64_t avg_wait_ms  = (total_wait / total_ops) / 1000000;
    int64_t avg_work_ms  = (total_work / total_ops) / 1000000;

    pg_actual(ctx, "wall_ms_approx", wall_ms);
    pg_gauge (ctx, "avg_wait_ms",    avg_wait_ms);
    pg_gauge (ctx, "avg_work_ms",    avg_work_ms);
    pg_count (ctx, "total_ops",      total_ops);
    pg_logf  (ctx, "wall=%lld ms, avg wait=%lld ms, avg work=%lld ms (fsync delay was %d ms)",
              (long long)wall_ms, (long long)avg_wait_ms, (long long)avg_work_ms,
              (int)(FSYNC_DELAY / 1000000));
    pg_logf  (ctx, "%d workers × %d ops → 48 operations queued through one 30ms critical section.",
              WORKERS, ITERS);

    chaos_io_close(io_fd);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "thundering_herd",
    .title       = "Thundering herd: lock + fsync",
    .one_liner   = "8 threads race for a mutex holding slow IO",
    .description = "Many workers wake simultaneously and queue through one mutex whose critical "
                   "section is a slow fsync. Total wall time ≈ N×iters×fsync.",
    .expected    = "avg wait grows with N; wall time ≈ fully serialized",
    .lesson      =
        "Why concurrent code runs serially: N threads all want the same\n"
        "lock. They acquire serially; while one holds it, N-1 wait. Total\n"
        "time = N × (critical section time). If the critical section\n"
        "includes slow IO, latency explodes linearly with concurrency.\n"
        "CPU usage looks low while the app is throughput-bound.\n"
        "\n"
        "Fixes:\n"
        "  - Shrink the critical section: do slow work OUTSIDE the lock.\n"
        "  - Shard the lock: many mutexes, hash requests across them.\n"
        "  - Lock-free data structures where contention is high.\n"
        "  - Connection/resource pooling — don't lock on the hot path.\n"
        "  - Backpressure: reject excess load rather than queue it.",
    .category    = PG_CAT_COMPOUND,
    .run         = run,
};

PG_SCENARIO_REGISTER(scen);
