# Architecture

## Process model

```
┌─────────────────────┐    fixed-size event records (256B)    ┌─────────────────────┐
│  parent: ncurses    │ ◄────────────────────────────────────  │  child: scenario    │
│  TUI + runner       │                                        │  + chaos primitives │
│                     │            fork() boundary             │                     │
└─────────────────────┘                                        └─────────────────────┘
   crashes never                                               SIGSEGV/SIGBUS/SIGABRT
   reach here                                                  → handler emits FAULT
                                                               → re-raises to die cleanly
```

Each scenario run is a `fork()`. The child:
1. Installs an async-signal-safe handler for SIGSEGV/SIGBUS/SIGABRT/SIGFPE/SIGILL that writes a final `PG_EV_FAULT` event before re-raising the signal.
2. Sets `RLIMIT_CPU` (30 s) so a runaway scenario can't lock the box.
3. Calls `scenario.init() → run() → teardown()`, then emits `PG_EV_DONE` and `_exit(0)`.

The parent:
1. Reads fixed-size 256-byte event records from the pipe (loops on partial reads).
2. Polls `{stdin, event_fd}` so it never blocks on either.
3. On user abort, sends `SIGTERM` to the child; reaps via `waitpid`.

This is the load-bearing safety decision. Alternatives considered:

- **Signal handlers + `sigjmp_buf`**: cheapest, but `longjmp` out of a SIGSEGV handler in a multi-threaded scenario is effectively undefined behavior. Rejected.
- **Sandbox arenas only**: doesn't help for stack overflows, ABRT, FPE, or any crash unrelated to memory. Rejected.
- **`fork` (chosen)**: ~5 ms overhead — noise vs. second-long scenarios — and bulletproof crash isolation. The child can also be capped with `setrlimit`, and we get clean post-mortem exit-status semantics.

## Components

| Path | Role |
|---|---|
| `src/main.c` | argument parsing, dispatch to TUI or `--scenario` headless mode |
| `src/core/registry.c` | constructor-driven plug-in registry (no central list) |
| `src/core/runner.c` | fork, IPC pump, abort, reap |
| `src/core/rand.c` | seeded xoshiro256\*\* (deterministic across runs) |
| `src/tui/tui.c` | ncurses 3-pane layout + state machine + lesson overlay |
| `src/chaos/chaos_thread.c` | spawn-with-tag, watchdog, cancel-at-time |
| `src/chaos/chaos_mem.c` | mmap'd arena with redzones + double-free + UAF poison |
| `src/chaos/chaos_net.c` | socketpair + proxy thread with loss/delay/fragmentation |
| `src/chaos/chaos_io.c` | wrapped fd ops (quota/partial/fsync-delay) + clock skew/jitter |
| `src/scenarios/<cat>/*.c` | one scenario per file; auto-discovered by CMake |

## The scenario contract

```c
typedef struct {
    const char     *id, *title, *one_liner, *description, *expected, *lesson;
    pg_category_t   category;
    int  (*init)    (pg_runctx_t *ctx, void **state);
    int  (*run)     (pg_runctx_t *ctx, void  *state);
    void (*teardown)(pg_runctx_t *ctx, void  *state);
    void (*explain) (pg_runctx_t *ctx, void  *state);
} pg_scenario_t;

#define PG_SCENARIO_REGISTER(desc)                                     \
    __attribute__((constructor)) static void pg__reg_##desc(void) {    \
        pg_registry_add(&desc);                                        \
    }                                                                  \
    struct pg__reg_##desc##__eat_semi
```

The constructor attribute means each `.c` file registers itself at startup. CMake's `file(GLOB_RECURSE)` picks up new files at configure time. Adding a scenario is a one-file change.

## Event protocol

```c
typedef struct {
    uint32_t kind;          /* pg_event_kind_t */
    uint32_t thread_tag;
    uint64_t ts_ns;         /* relative to scenario start */
    int64_t  num;
    char     key [32];
    char     text[192];
    uint8_t  _reserved[8];  /* pad to 256 */
} pg_event_t;

_Static_assert(sizeof(pg_event_t) == 256, "pg_event_t must be exactly 256 bytes");
```

Fixed size matters: a child that crashes mid-`write()` produces a partial record. The parent loops `read()` until it has the full 256 bytes, so the next event always lands on a 256-byte boundary. POSIX guarantees writes ≤ `PIPE_BUF` (≥ 512 on macOS/Linux) are atomic on pipes, so concurrent emits from multiple threads in the child are safe without locks.

## Why chaos in-process?

We deliberately do *not* call `tc`, `pfctl`, or send real signals to other PIDs. Trade-offs:

- **No sudo, no platform-specific privilege juggling, no risk of hosing the system.**
- **The chaos is still real where it matters**: real `pthread_cancel` (chaos_thread), real OOB writes into mmap'd pages (chaos_mem), real `socketpair` byte streams with synthetic loss in the proxy thread (chaos_net), real fds backed by temp files with policy on top (chaos_io).
- **The user code is unchanged from real-network/real-disk code** — `read`/`write`/`select`/`poll`/`fsync` all behave normally; the chaos is at the wrapper level.

## Sanitizers

CMake build types `Asan`, `Tsan`, `Ubsan` are wired up:
```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan && cmake --build build-tsan
./build-tsan/playground_run --scenario race_counter   # TSan reports the race
./build-asan/playground_run --scenario uaf_callback   # ASan would catch UAF earlier than the arena does
```

## Determinism

`pg_runctx_t` carries a seeded `pg_xosh_t` (xoshiro256\*\*). Scenarios that draw from `pg_runctx_rand(ctx)` are reproducible across runs. Scenarios that rely on actual OS scheduling (race_counter, deadlock_classic, jittery_timer) are intentionally non-deterministic — that's the failure being demonstrated.

## Platform notes

- **macOS arm64 (primary)**: clang, system ncurses, `clock_gettime(CLOCK_MONOTONIC)`, kqueue available for future watchdog work. Works as-is.
- **Linux (secondary)**: same code path; `timer_create` is available if we want a kernel-driven watchdog. Currently we use a thread + `nanosleep`, which is portable.
- **Windows (out of scope)**: `fork`, `pthread_cancel`, signal model, mmap semantics — too many divergences.
