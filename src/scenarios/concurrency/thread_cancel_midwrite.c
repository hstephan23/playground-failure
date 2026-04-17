#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
    int a, b, c, d, e, f, g, h;
} record_t;

static record_t g_dst;

static void *copier_fn(void *arg) {
    (void)arg;
    record_t src = { 11, 22, 33, 44, 55, 66, 77, 88 };
    /* deliberate: usleep is a cancellation point. cancel will fire mid-copy. */
    for (;;) {
        g_dst.a = src.a; usleep(200);
        g_dst.b = src.b; usleep(200);
        g_dst.c = src.c; usleep(200);
        g_dst.d = src.d; usleep(200);
        g_dst.e = src.e; usleep(200);
        g_dst.f = src.f; usleep(200);
        g_dst.g = src.g; usleep(200);
        g_dst.h = src.h; usleep(200);
    }
    return NULL;
}

static int tcm_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    g_dst = (record_t){0};

    pg_phase(ctx, "spawning copier; will cancel after 1ms (mid-record)");
    chaos_thread_t *t = chaos_thread_spawn(copier_fn, NULL, "copier");
    chaos_thread_cancel_at_ns(t, 1000000ULL);    /* 1 ms */
    chaos_thread_join(t);

    pg_actual(ctx, "field_a", g_dst.a);
    pg_actual(ctx, "field_b", g_dst.b);
    pg_actual(ctx, "field_c", g_dst.c);
    pg_actual(ctx, "field_d", g_dst.d);
    pg_actual(ctx, "field_e", g_dst.e);
    pg_actual(ctx, "field_f", g_dst.f);
    pg_actual(ctx, "field_g", g_dst.g);
    pg_actual(ctx, "field_h", g_dst.h);

    int set = 0;
    if (g_dst.a) set++; if (g_dst.b) set++; if (g_dst.c) set++; if (g_dst.d) set++;
    if (g_dst.e) set++; if (g_dst.f) set++; if (g_dst.g) set++; if (g_dst.h) set++;
    pg_expect(ctx, "fields_set", 8);
    pg_actual(ctx, "fields_set", set);

    pg_logf(ctx, "after cancellation, %d/8 fields contain real data; %d are zero/torn",
            set, 8 - set);
    pg_logf(ctx, "another thread reading the struct now sees a half-constructed value.");
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "thread_cancel_midwrite",
    .title       = "Cancel mid-write: torn struct",
    .one_liner   = "pthread_cancel a thread copying a struct",
    .description = "A thread copies an 8-field struct field-by-field. We cancel it 1ms in. Reader sees a half-written struct.",
    .expected    = "fewer than 8 fields populated; readers see torn state",
    .lesson      =
        "Why this is dangerous: pthread_cancel can interrupt a thread at any\n"
        "cancellation point (read, write, sleep, mutex_timedlock, ...). If the\n"
        "thread was mid-update of a struct, fields are half-written. Other\n"
        "threads see torn state with no warning.\n"
        "\n"
        "Fixes:\n"
        "  - Disable cancellation around critical sections:\n"
        "      pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old)\n"
        "  - Use pthread_cleanup_push to register rollback handlers.\n"
        "  - Avoid pthread_cancel; use a graceful shutdown flag the thread\n"
        "    polls between operations instead.",
    .category    = PG_CAT_CONCURRENCY,
    .run         = tcm_run,
};

PG_SCENARIO_REGISTER(scen);
