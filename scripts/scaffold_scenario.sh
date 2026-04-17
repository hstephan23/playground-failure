#!/usr/bin/env bash
#
# scaffold_scenario.sh — stamp out a new failure-playground scenario.
#
# Usage: scripts/scaffold_scenario.sh <category> <scenario_id>
#
#   category    one of: concurrency, memory, network, timeio
#   scenario_id snake_case identifier ([a-z][a-z0-9_]*)
#
# Creates src/scenarios/<category>/<scenario_id>.c from a template.
# Refuses to overwrite an existing file.
#
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") <category> <scenario_id>

Categories : concurrency, memory, network, timeio
ID         : snake_case ([a-z][a-z0-9_]*)

Examples:
  $(basename "$0") memory   leak_slow
  $(basename "$0") network  reorder
  $(basename "$0") timeio   slow_open
EOF
    exit 1
}

[[ $# -eq 2 ]] || usage

cat="$1"
id="$2"

case "$cat" in
    concurrency|memory|network|timeio) ;;
    *) echo "error: unknown category '$cat'" >&2; usage ;;
esac

if ! [[ "$id" =~ ^[a-z][a-z0-9_]*$ ]]; then
    echo "error: scenario id must match [a-z][a-z0-9_]* — got '$id'" >&2
    exit 1
fi

# resolve project root from the script's location (works regardless of cwd)
script_dir="$(cd "$(dirname "$0")" && pwd)"
proj_root="$(cd "$script_dir/.." && pwd)"
target_dir="$proj_root/src/scenarios/$cat"
target="$target_dir/$id.c"

mkdir -p "$target_dir"

if [[ -e "$target" ]]; then
    echo "error: $target already exists — refusing to overwrite" >&2
    exit 1
fi

# uppercase category for the PG_CAT_* enum
cat_upper="$(printf '%s' "$cat" | tr '[:lower:]' '[:upper:]')"

cat > "$target" <<EOF
#include "playground/scenario.h"
#include "playground/event.h"

/* Pull in any chaos primitives you need:
 *   #include "playground/chaos_thread.h"
 *   #include "playground/chaos_mem.h"
 *   #include "playground/chaos_net.h"
 *   #include "playground/chaos_io.h"
 */

static int run(pg_runctx_t *ctx, void *state) {
    (void)state;

    pg_phase(ctx, "TODO: phase 1");
    pg_logf (ctx, "TODO: emit an interesting log line");

    /* TODO: actually break something here. */

    pg_expect(ctx, "result", 1);
    pg_actual(ctx, "result", 0);   /* TODO: replace with the real measurement */
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "$id",
    .title       = "TODO: short human title",
    .one_liner   = "TODO: one-line teaser shown in the menu",
    .description = "TODO: one or two sentences of longer description",
    .expected    = "TODO: what should the user observe",
    .lesson      =
        "TODO: explain WHY this failure happens.\n"
        "\n"
        "Fixes:\n"
        "  - TODO\n"
        "  - TODO",
    .category    = PG_CAT_${cat_upper},
    .run         = run,
};

PG_SCENARIO_REGISTER(scen);
EOF

# nice relative path for the human
rel_target="${target#$proj_root/}"

cat <<EOF
created $rel_target

Next steps:
  1. fill in the TODOs in $rel_target
  2. cmake --build build
  3. ./build/playground_run --scenario $id
  4. ./build/playground   (it will appear in the menu)
EOF
