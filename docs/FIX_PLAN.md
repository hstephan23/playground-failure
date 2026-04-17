# Playground-Failure — Design Review Fix Plan

A phased plan for addressing the issues surfaced in the design review. Ordered
by dependency: Phase 0 lands a safety net first, Phase 1 fixes real correctness
bugs, Phase 2 changes the event/fault model (with regenerated goldens), Phase 3
cleans up API honesty, and Phase 4 is hardening and polish.

Each item names the exact file(s) to touch and how to verify the change.

---

## Phase 0 — Lock in current behavior before changing anything

Nothing here changes behavior. The goal is a safety net before we touch the
wire format or the fault model.

### 0.1 Golden-file test harness

Create `tests/golden/run.sh`. For each registered scenario, invoke:

```
./build/playground_run --scenario $ID --seed 42 --json
```

and diff against `tests/golden/<id>.jsonl`. Event timestamps are relative to
scenario start and depend on the real clock, so either round `ts_ns` to a
bucket (`ts_ns / 1_000_000`) before diffing, or strip `ts_ns` entirely and rely
on `kind`/`key`/`num`/`text` ordering.

Wire as a CMake `add_test` so `ctest` runs it.

This captures *current* output; Phase 2 regenerates the goldens intentionally
when the event model changes.

### 0.2 Per-module unit tests

Create `tests/unit/test_arena.c`, `test_io.c`, `test_net.c`, `test_thread.c`.
Minimum coverage:

- arena double-free returns `-1`
- arena redzone corruption is detected by `chaos_arena_check`
- `chaos_io_write` honors quota (returns `-1`/`ENOSPC` once exhausted)
- `chaos_net_pair` delivers uncorrupted bytes when knobs are zero

Tiny `int main(void)` programs that link the same object files and return
non-zero on failure. No framework required.

### Verification

`cmake --build build && ctest --test-dir build` runs clean on all sanitizer
build types (`Debug`, `ASAN`, `TSAN`, `UBSAN`). TSan will fail until Phase 1.

---

## Phase 1 — Correctness bugs

### 1.1 Fix the data race on `chaos_net`'s `running` flag

**File:** `src/chaos/chaos_net.c` — field on line 22, `while (I->running)` on
line 53, stores in the close path.

Preferred fix: **delete the flag entirely** and rely on EOF from
`close(p->fd_a/fd_b)` to wake the forwarders. Removes a synchronization point
and is simpler.

Alternative: make it `_Atomic int` with `atomic_load_explicit(...,
memory_order_acquire)` / `atomic_store_explicit(..., memory_order_release)`.

**Verify:** TSan build runs `packet_loss` clean.

### 1.2 Harden `pg_emit` against short/interrupted writes

**File:** `src/core/runner.c` — `pg_emit` (~line 56).

Current `(void)w` silently drops events on EINTR or write errors. Loop:

```c
size_t off = 0;
const uint8_t *p = (const uint8_t *)&copy;
while (off < sizeof(copy)) {
    ssize_t w = write(ctx->event_fd_w, p + off, sizeof(copy) - off);
    if (w < 0) {
        if (errno == EINTR) continue;
        return;  /* pipe broken; parent has gone */
    }
    off += (size_t)w;
}
```

On a blocking pipe with writes ≤ PIPE_BUF, a partial write only happens via
signal — in which case POSIX returns either full or zero (with EINTR). The
loop therefore preserves the no-interleaving property. Update the comment.

The signal-handler path in `child_fault_handler` keeps its single best-effort
`write`. Document as "best effort; if the pipe is broken we still exit".

**Verify:** existing goldens diff clean. Add a unit test that forks a child
emitting 10,000 events as fast as possible while the parent deliberately
throttles reads; assert all 10,000 arrive in order.

### 1.3 Fix the `chaos_clock` determinism leak

**File:** `src/chaos/chaos_io.c` — line 157 (`g_clock_rng = { .s = { 1, 2, 3, 4 } }`).

Add `chaos_clock_seed(uint64_t seed)` to the public header. Call it from
`pg_run_start` just before invoking the scenario's `init`, passing
`seed ^ 0xC10CC10CC10CC10CULL` (distinct per-subsystem constant, matching the
`chaos_net` pattern). Jitter becomes reproducible.

**Verify:** `playground_run --scenario slow_fsync --seed 42` produces
byte-identical output across two runs (after Phase 2, strip `ts_ns` from the
diff).

### 1.4 Replace `pthread_cancel` with a cooperative stop flag

**Files:** `include/playground/chaos_thread.h`, `src/chaos/chaos_thread.c`.

`pthread_cancel` is fragile on macOS and only fires at cancellation points —
scenarios in tight compute loops or holding mutexes won't be cancellable.

Add `_Atomic int stop_flag` to `struct chaos_thread`. Expose
`chaos_thread_should_stop(chaos_thread_t *)`. `chaos_thread_cancel` sets the
flag (keep `pthread_cancel` as a best-effort secondary call). Same for
`cancel_at_ns`. Document in the header that scenarios must poll at logical
breakpoints. `_exit(2)` via watchdog remains the hard backstop.

**Verify:** a new concurrency scenario that spins in a compute loop and polls
the flag exits cleanly under `cancel_at_ns`. The same scenario with polling
removed hangs until the watchdog fires.

### 1.5 Fix `chaos_thread_join` double-free potential

**File:** `src/chaos/chaos_thread.c`.

The `joined` field is written but never read, so a second join on the same
handle is UAF.

Preferred: **remove the `joined` field**, document "handle is invalid after
return" in the header, and match how scenarios actually use it.

Alternative: guard with `if (t->joined) return -1;` and split ownership into
`chaos_thread_join` + `chaos_thread_release`.

**Verify:** unit test asserts single-join-per-handle contract.

---

## Phase 2 — Semantic fixes to the event/fault model

### 2.1 Split `PG_EV_FAULT` into two kinds

**Files:** `include/playground/event.h`, all emit sites.

Introduce:

- `PG_EV_SUT_FAULT` — the system under test misbehaved in the way the
  scenario was demonstrating.
- `PG_EV_CHILD_CRASH` — the child process died from a signal.

Rename `pg_fault` → `pg_sut_fault`. Signal handler emits
`PG_EV_CHILD_CRASH`. Watchdog is a harness-detected hang — either add a
third kind `PG_EV_WATCHDOG` or fold into `SUT_FAULT` with `key="watchdog"`.

Update:
- `src/scenarios/memory/double_free.c`
- `src/scenarios/concurrency/deadlock_classic.c`
- other scenarios that emit faults
- `pg_event_kind_name`
- TUI's `format_event_line` switch
- JSON emitter in `src/main.c`
- regenerate goldens

**Verify:** for each scenario, confirm that `SUT_FAULT` marks the buggy
behavior and `CHILD_CRASH` only fires on real signal-level crashes (e.g.
`crash_null_deref`).

### 2.2 Add a version byte to `pg_event_t`

**File:** `include/playground/event.h`.

Steal from the 8 reserved padding bytes:

```c
typedef struct {
    uint16_t magic;      /* = PG_EVENT_MAGIC (0xE5E7) */
    uint8_t  version;    /* = 1 */
    uint8_t  _pad0;
    uint32_t kind;
    ...
    uint8_t  _reserved[4];  /* was 8 */
} pg_event_t;
```

`sizeof == 256` and the `_Static_assert` are preserved. `pg_emit` sets
magic/version on every emit (zero cost). Parent side in `pg_run_poll` checks
magic/version and returns `-1` with a log line on mismatch — turning "silent
misalignment after a pipe crash" into a loud failure. Future format changes
bump `version`.

**Verify:** unit test constructs a 256-byte buffer with wrong magic, feeds it
through a mocked pipe, asserts `pg_run_poll` returns `-1`.

---

## Phase 3 — API honesty

### 3.1 Honor `chaos_io_open`'s `flags` (or drop the parameter)

**Files:** `src/chaos/chaos_io.c`, `include/playground/chaos_io.h`.

Two options:

- **Honor:** `mkstemp` to get a path, `close` its fd, then `open(path, flags |
  O_CREAT, 0600)` with the caller's flags and unlink. Downside: brief TOCTOU.
- **Drop:** remove `flags` from the signature, declare
  `chaos_io_open(const char *name)`, update scenario callers in
  `src/scenarios/timeio/*.c`, document "flags are fixed at O_RDWR|O_CREAT".

Pick dropping unless a scenario actually needs e.g. `O_APPEND` or `O_DIRECT`.

### 3.2 Tighten `chaos_arena_create` on small requests

**File:** `src/chaos/chaos_mem.c` — line 38.

Return `NULL` with `errno = EINVAL` for `bytes < 4096` instead of silently
bumping. Scenarios always pass ≥ 4096 today.

### 3.3 Handle `--seed 0` at the CLI layer

**File:** `src/main.c`.

`pg_xosh_seed` silently remaps 0 to `0xDEADBEEFDEADBEEF`. A user typing
`--seed 0` expecting the all-zeros state gets something else. Print a warning
(friendlier) or fail hard.

### 3.4 Make `RLIMIT_CPU` configurable

**File:** `src/core/runner.c` — line 209.

Add env var `PG_CPU_LIMIT_SECONDS` (default 30). Some time/IO scenarios could
plausibly want more.

---

## Phase 4 — Hardening and polish

### 4.1 Embed forwarder args in `pair_internal_t`

**File:** `src/chaos/chaos_net.c` — lines 24–29, 115–139.

Embed the two `forward_arg_t` structs directly in `pair_internal_t`. Removes
two `malloc`s, two free paths, and the ownership transfer.

### 4.2 Per-fd locking in `chaos_io`

Optional. If you add IO contention scenarios, replace the single global
mutex with per-slot mutexes. Not needed for current scenarios.

### 4.3 Tidy the child signal handler

**File:** `src/core/runner.c` — lines 159–160.

`SA_RESETHAND` is already set; the explicit `signal(sig, SIG_DFL)` before
`raise` is redundant. Drop it.

### 4.4 Document `teardown`'s best-effort semantics

**File:** `include/playground/scenario.h`.

Add a header comment on the `teardown` field:

> Called on the normal run path only; not guaranteed to run if `run` crashes
> or the watchdog fires. Do not rely on it for resources outside the
> scenario's own process lifetime.

### 4.5 Populate `docs/`

Short top-level README covering:

- The fork-per-scenario model.
- The 256-byte event wire format (including magic/version from 2.2).
- Build types and what each sanitizer catches.
- How to add a scenario (one `.c` file with `PG_SCENARIO_REGISTER`).

Per-scenario docs stay in the `explain()` callback. Architectural docs live
here.

### 4.6 Encapsulate TUI globals

**File:** `src/tui/tui.c`.

Wrap `g_log`, `g_stats`, the windows, and `g_scens` into a
`typedef struct tui_state { ... }` and thread a pointer through. Cosmetic;
low priority.

---

## Suggested commit sequence

One PR per phase, in order:

1. **PR #1 (Phase 0):** "Add golden and unit test harness" — no behavior
   changes, green CI everywhere.
2. **PR #2 (Phase 1):** "Fix correctness bugs: data race, short writes,
   determinism, cancellation" — goldens unchanged, TSan now passes, unit
   tests added.
3. **PR #3 (Phase 2):** "Event model v1: split fault kinds, add
   magic/version" — goldens regenerated; that regeneration is the point of
   the review.
4. **PR #4 (Phase 3):** "API honesty cleanups" — small, reviewable, goldens
   touched where scenarios change output.
5. **PR #5 (Phase 4):** "Hardening and docs" — grab-bag, non-urgent.

## Rough effort estimate

| Phase | Effort         |
| ----- | -------------- |
| 0     | ~½ day         |
| 1     | ~1 day         |
| 2     | ~½ day         |
| 3     | ~2 hours       |
| 4     | flexible       |
