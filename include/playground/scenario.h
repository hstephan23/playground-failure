#pragma once

/* The plug-in contract for failure scenarios.
 *
 * A scenario is a translation unit that defines a static `pg_scenario_t`
 * describing itself, then registers it via PG_SCENARIO_REGISTER(). The
 * registration runs as a constructor at process startup, so adding a new
 * scenario is a single-file change — no central list to edit.
 *
 * At runtime, the parent process forks a child for each scenario run; the
 * scenario's run() function executes inside that child and emits events
 * through pg_emit / pg_logf / pg_count / pg_gauge / pg_expect / pg_actual /
 * pg_sut_fault (declared in event.h). Any crash in the scenario stays inside
 * the child — see runner.h. */

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PG_CAT_CONCURRENCY = 0,
    PG_CAT_MEMORY,
    PG_CAT_NETWORK,
    PG_CAT_TIMEIO,
    PG_CAT__COUNT
} pg_category_t;

const char *pg_category_name(pg_category_t cat);

typedef struct pg_runctx pg_runctx_t;

typedef struct {
    const char     *id;
    const char     *title;
    const char     *one_liner;
    const char     *description;
    const char     *expected;
    /* Multi-line prose shown in the TUI's explain overlay. Use \n to break lines.
     * Optional; NULL means "no overlay content". */
    const char     *lesson;
    pg_category_t   category;

    int  (*init)    (pg_runctx_t *ctx, void **state);
    int  (*run)     (pg_runctx_t *ctx, void  *state);
    /* Optional best-effort cleanup. Called only on the normal run path; not
     * guaranteed to run if run() crashes (signal kills the child) or if the
     * watchdog fires. Don't use for resources that outlive the scenario
     * process — the OS will reclaim those anyway. */
    void (*teardown)(pg_runctx_t *ctx, void  *state);
    void (*explain) (pg_runctx_t *ctx, void  *state);
} pg_scenario_t;

#define PG_SCENARIO_REGISTER(desc)                                            \
    __attribute__((constructor)) static void pg__reg_##desc(void) {           \
        pg_registry_add(&desc);                                               \
    }                                                                         \
    struct pg__reg_##desc##__eat_semi /* swallow trailing ; */

void                  pg_registry_add (const pg_scenario_t *s);
const pg_scenario_t **pg_registry_list(size_t *out_n);
const pg_scenario_t  *pg_registry_find(const char *id);

uint64_t pg_runctx_seed(const pg_runctx_t *ctx);
uint64_t pg_runctx_rand(pg_runctx_t *ctx);
