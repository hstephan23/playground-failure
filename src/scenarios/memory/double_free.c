#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_mem.h"

#include <stdint.h>

static int df_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    chaos_arena_t *a = chaos_arena_create(64 * 1024, CHAOS_ARENA_REDZONES);
    if (!a) { pg_fault(ctx, "arena create failed"); return 1; }

    pg_phase(ctx, "alloc 128 bytes");
    void *p = chaos_arena_alloc(a, 128);
    pg_logf(ctx, "alloc returned %p", p);

    pg_phase(ctx, "first free (legitimate)");
    int r1 = chaos_arena_free(a, p);
    pg_logf(ctx, "first free: rc=%d  (0 = ok)", r1);

    pg_phase(ctx, "second free of the SAME pointer");
    int r2 = chaos_arena_free(a, p);
    pg_logf(ctx, "second free: rc=%d  (-1 = arena detected double-free)", r2);

    pg_expect(ctx, "double_free_caught", 1);
    pg_actual(ctx, "double_free_caught", r2 == -1 ? 1 : 0);

    if (r2 == -1) {
        pg_fault(ctx, "double-free detected");
        pg_logf(ctx, "with libc malloc this would corrupt the freelist; the next");
        pg_logf(ctx, "alloc could return a pointer someone else still uses.");
    }

    chaos_arena_destroy(a);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "double_free",
    .title       = "Double-free detected",
    .one_liner   = "free(p); free(p) — arena catches it",
    .description = "Frees the same pointer twice; the arena's per-block magic catches the second free.",
    .expected    = "second free returns -1; PG_EV_FAULT emitted",
    .lesson      =
        "Why this corrupts: most production allocators don't check magic on\n"
        "free. The second free corrupts the freelist; the next alloc may\n"
        "return a pointer that someone else still uses. Crashes happen\n"
        "later, far from the cause -- the worst kind of bug to debug.\n"
        "\n"
        "Fixes:\n"
        "  - Set the pointer to NULL after free. free(NULL) is a no-op, so\n"
        "    a second 'free' is harmless.\n"
        "  - Use a smart-pointer / unique-ownership pattern (each object has\n"
        "    one owner who frees it).\n"
        "  - AddressSanitizer catches double-free in dev/CI.",
    .category    = PG_CAT_MEMORY,
    .run         = df_run,
};

PG_SCENARIO_REGISTER(scen);
