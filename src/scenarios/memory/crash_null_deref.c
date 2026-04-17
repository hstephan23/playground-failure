#include "playground/scenario.h"
#include "playground/event.h"

static int cn_run(pg_runctx_t *ctx, void *state) {
    (void)state;
    pg_phase(ctx, "about to dereference NULL");
    pg_logf(ctx, "the runner's signal handler should catch SIGSEGV,");
    pg_logf(ctx, "emit PG_EV_FAULT(\"child crashed: SIGSEGV\"), and re-raise.");
    pg_logf(ctx, "the parent TUI must survive.");

    /* volatile so the optimizer doesn't elide it */
    *(volatile int *)0 = 0xDEAD;

    pg_logf(ctx, "(unreachable)");
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "crash_null_deref",
    .title       = "Crash: NULL deref (containment proof)",
    .one_liner   = "writes to *NULL — should not take down the TUI",
    .description = "Deliberately segfaults to prove the fork-per-scenario isolation works.",
    .expected    = "PG_EV_FAULT(SIGSEGV); child exits via signal; parent unaffected",
    .lesson      =
        "This scenario exists to PROVE the runner contains crashes. The\n"
        "child segfaults; the runner's signal handler emits a fault event\n"
        "then re-raises. The default handler kills the child. The parent\n"
        "TUI keeps running and reaps the abnormal exit.\n"
        "\n"
        "Real NULL derefs in C come from:\n"
        "  - forgetting to check return values (malloc, fopen, getenv)\n"
        "  - stale pointers after a struct was reallocated (realloc moves)\n"
        "  - off-by-one in an array of pointers\n"
        "  - aliased pointers where one path frees and another reads",
    .category    = PG_CAT_MEMORY,
    .run         = cn_run,
};

PG_SCENARIO_REGISTER(scen);
