#!/usr/bin/env bash
#
# tests/golden/run.sh <playground_run> <golden_dir> [--regenerate]
#
# Runs deterministic scenarios via playground_run with --seed 42 --json,
# strips ts_ns (which depends on real clock), and either diffs against the
# checked-in golden file or regenerates it.
#
set -euo pipefail

binary="${1:?usage: run.sh <playground_run> <golden_dir> [--regenerate]}"
gdir="${2:?golden dir required}"
mode="${3:-check}"

# Only deterministic scenarios. Race-driven and timing-driven scenarios are
# excluded from goldens (they would fail intermittently); they're covered by
# smoke runs and unit tests instead.
scenarios=(
    uaf_callback
    double_free
    arena_corrupt
    crash_null_deref
    partial_write
    disk_full
    clock_jump
)

fail=0
for id in "${scenarios[@]}"; do
    expected_file="$gdir/$id.jsonl"
    # Normalize away things that legitimately vary between runs:
    #   ts_ns        — depends on real clock
    #   0xHEX        — pointer addresses (ASLR)
    actual="$("$binary" --scenario "$id" --seed 42 --json 2>/dev/null \
              | sed -E 's/"ts_ns":[0-9]+,?//; s/0x[0-9a-fA-F]+/0xPTR/g')"

    if [[ "$mode" == "--regenerate" ]]; then
        printf '%s\n' "$actual" > "$expected_file"
        echo "regenerated: $expected_file"
        continue
    fi

    if [[ ! -f "$expected_file" ]]; then
        echo "MISSING golden: $expected_file" >&2
        fail=1
        continue
    fi

    if ! diff -u "$expected_file" <(printf '%s\n' "$actual") >&2; then
        echo "MISMATCH: $id" >&2
        fail=1
    fi
done

if [[ $fail -eq 0 ]]; then
    echo "golden: ${#scenarios[@]} scenarios match"
fi
exit $fail
