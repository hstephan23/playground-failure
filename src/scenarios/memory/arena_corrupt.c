#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_mem.h"

#include <stdint.h>
#include <string.h>

static int ac_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    chaos_arena_t *a = chaos_arena_create(64 * 1024, CHAOS_ARENA_REDZONES);
    if (!a) { pg_fault(ctx, "arena create failed"); return 1; }

    pg_phase(ctx, "alloc 32 bytes (with 16B redzones front & back)");
    char *p = (char *)chaos_arena_alloc(a, 32);

    pg_phase(ctx, "write 64 bytes — that's 32 bytes off the end");
    memset(p, 'X', 64);

    pg_phase(ctx, "scan arena for redzone corruption");
    int corrupted = chaos_arena_check(a);
    pg_expect(ctx, "redzones_corrupt", 1);
    pg_actual(ctx, "redzones_corrupt", corrupted);

    if (corrupted > 0) {
        pg_fault(ctx, "out-of-bounds write detected by redzone scan");
        pg_logf (ctx, "without redzones, this write would silently corrupt the next allocation.");
        pg_logf (ctx, "the bug would manifest as a crash or wrong value far from the cause.");
    } else {
        pg_logf(ctx, "(unexpected) redzones still intact");
    }

    chaos_arena_destroy(a);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "arena_corrupt",
    .title       = "OOB write: redzone caught",
    .one_liner   = "write 64B into a 32B alloc; redzone catches it",
    .description = "A classic buffer overflow inside the chaos arena. Redzones detect it; in production this would silently trash adjacent memory.",
    .expected    = "redzones_corrupt = 1; PG_EV_FAULT emitted",
    .lesson      =
        "Why this is silent without instrumentation: the OOB bytes land in\n"
        "whatever's allocated next — another struct, another buffer. That\n"
        "code reads corrupted data later. Crashes happen far from the bug.\n"
        "\n"
        "Fixes:\n"
        "  - Use AddressSanitizer (-fsanitize=address) in dev/CI; it adds\n"
        "    real-time redzones around every malloc.\n"
        "  - Use bounded APIs (snprintf, strlcpy, memcpy_s) instead of\n"
        "    strcpy/sprintf.\n"
        "  - Validate size BEFORE writing.",
    .category    = PG_CAT_MEMORY,
    .run         = ac_run,
};

PG_SCENARIO_REGISTER(scen);
