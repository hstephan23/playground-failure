#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_mem.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int   secret;
    int   refcount;
    char  name[32];
} closure_t;

static int uc_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    chaos_arena_t *a = chaos_arena_create(64 * 1024, CHAOS_ARENA_REDZONES);
    if (!a) { pg_fault(ctx, "arena create failed"); return 1; }

    pg_phase(ctx, "alloc closure");
    closure_t *c = (closure_t *)chaos_arena_alloc(a, sizeof(*c));
    c->secret   = 0xCAFE;
    c->refcount = 1;
    snprintf(c->name, sizeof(c->name), "callback-state");

    pg_logf(ctx, "before free: secret=0x%04x refcount=%d name=\"%s\"",
            c->secret, c->refcount, c->name);
    pg_expect(ctx, "secret", 0xCAFE);

    pg_phase(ctx, "free closure (other code still holds the pointer)");
    chaos_arena_free(a, c);

    pg_phase(ctx, "use after free -- read the dangling pointer");
    /* memory was poisoned to 0xDE on free, so reads now return obvious garbage */
    pg_logf(ctx, "after free:  secret=0x%04x refcount=%d name=\"%.16s\"",
            (unsigned)c->secret & 0xFFFF, c->refcount, c->name);
    pg_actual(ctx, "secret", (int64_t)((unsigned)c->secret & 0xFFFF));
    pg_logf(ctx, "the value silently changed; if `name` were the only valid string in a logger,");
    pg_logf(ctx, "you'd be writing 0xDE garbage to disk and never know.");

    chaos_arena_destroy(a);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "uaf_callback",
    .title       = "UAF: dangling closure",
    .one_liner   = "alloc, free, then read — see poison bytes",
    .description = "Allocates a struct, frees it, then reads the dangling pointer. Arena poisons freed memory to make UAF visible.",
    .expected    = "post-free read returns 0xDEDE garbage instead of 0xCAFE",
    .lesson      =
        "Why this is dangerous: a freed pointer still holds an address that\n"
        "looks valid. Reads return whatever bytes happen to be there now --\n"
        "often poison (here 0xDE) or, in production, data from a later\n"
        "allocation that got the same memory. The bug is invisible until\n"
        "something downstream uses the corrupted value.\n"
        "\n"
        "Fixes:\n"
        "  - Set the pointer to NULL right after free. Subsequent uses crash\n"
        "    loudly instead of returning garbage.\n"
        "  - Use refcounting / shared ownership for objects with multiple\n"
        "    holders.\n"
        "  - AddressSanitizer (ASan) catches UAF reliably in dev/CI:\n"
        "      cmake -B build-asan -DCMAKE_BUILD_TYPE=Asan",
    .category    = PG_CAT_MEMORY,
    .run         = uc_run,
};

PG_SCENARIO_REGISTER(scen);
