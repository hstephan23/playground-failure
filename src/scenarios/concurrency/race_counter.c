#include "playground/scenario.h"
#include "playground/event.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <unistd.h>

#define WRITERS  8
#define WRITES   1000

typedef struct {
    pg_runctx_t *ctx;
    int          tag;
    long        *counter;
} writer_arg_t;

static void *writer_fn(void *p) {
    writer_arg_t *w = (writer_arg_t *)p;
    for (int i = 0; i < WRITES; ++i) {
        long v = *w->counter;
        /* yield occasionally to widen the race window */
        if ((i & 0x3F) == 0) sched_yield();
        *w->counter = v + 1;
    }
    pg_logf(w->ctx, "writer %d finished", w->tag);
    return NULL;
}

static int rc_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    pg_phase (ctx, "spawning writers");
    pg_expect(ctx, "counter", (int64_t)WRITERS * WRITES);

    long         counter = 0;
    pthread_t    th[WRITERS];
    writer_arg_t args[WRITERS];

    for (int i = 0; i < WRITERS; ++i) {
        args[i].ctx     = ctx;
        args[i].tag     = i + 1;
        args[i].counter = &counter;
        if (pthread_create(&th[i], NULL, writer_fn, &args[i]) != 0) {
            pg_sut_fault(ctx, "pthread_create failed");
            return 1;
        }
        pg_logf(ctx, "spawned writer %d", i + 1);
    }

    pg_phase(ctx, "joining");
    for (int i = 0; i < WRITERS; ++i) pthread_join(th[i], NULL);

    pg_actual(ctx, "counter", (int64_t)counter);
    long expected = (long)WRITERS * WRITES;
    long lost     = expected - counter;
    pg_logf(ctx, "lost %ld of %ld writes (%.1f%%)",
        lost, expected, 100.0 * (double)lost / (double)expected);
    return 0;
}

static void rc_explain(pg_runctx_t *ctx, void *state) {
    (void)state;
    pg_logf(ctx, "Multiple threads execute `*c = *c + 1` without synchronization.");
    pg_logf(ctx, "The read-modify-write is three steps; another thread can write");
    pg_logf(ctx, "between read and write, so its increment is silently lost.");
    pg_logf(ctx, "Fix: pthread_mutex around the update, or atomic_fetch_add.");
}

static const pg_scenario_t scen = {
    .id          = "race_counter",
    .title       = "Race: lost increments",
    .one_liner   = "8 threads ++counter without locks",
    .description = "A canonical data race: threads read-modify-write a shared counter without synchronization.",
    .expected    = "counter = 8000; actual usually less",
    .lesson      =
        "Why this fails: `*counter = *counter + 1` is three operations: read,\n"
        "increment, write. Two threads can both read the same value, both\n"
        "increment to the same result, and both write back the same incremented\n"
        "value -- the second write silently overwrites the first.\n"
        "\n"
        "Fixes:\n"
        "  - pthread_mutex_lock around the update.\n"
        "  - atomic_fetch_add(&counter, 1) (C11 stdatomic.h).\n"
        "  - Single-writer architecture: one thread owns the value.\n"
        "\n"
        "TSan (clang -fsanitize=thread) reports the race exactly. Try:\n"
        "  cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan && cmake --build build-tsan\n"
        "  ./build-tsan/playground_run --scenario race_counter",
    .category    = PG_CAT_CONCURRENCY,
    .run         = rc_run,
    .explain     = rc_explain,
};

PG_SCENARIO_REGISTER(scen);
