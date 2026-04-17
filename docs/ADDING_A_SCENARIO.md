# Adding a Scenario

A scenario is a single `.c` file under `src/scenarios/<category>/`. CMake discovers it at configure time; the constructor-attribute registration auto-wires it into the menu at startup. There is no central list to edit.

## The fast way: scaffold script

```bash
./scripts/scaffold_scenario.sh memory my_new_thing
```

This stamps out `src/scenarios/memory/my_new_thing.c` from a template with TODOs for you to fill in. Then:

```bash
cmake --build build
./build/playground            # your scenario shows up in the menu
./build/playground_run --scenario my_new_thing
```

Categories: `concurrency`, `memory`, `network`, `timeio`.

## The manual way

1. Pick a `category` and a `snake_case_id`.
2. Create `src/scenarios/<category>/<id>.c`.
3. Define a `pg_scenario_t` and call `PG_SCENARIO_REGISTER(name)`.

Minimal compilable example:

```c
#include "playground/scenario.h"
#include "playground/event.h"

static int run(pg_runctx_t *ctx, void *state) {
    (void)state;
    pg_phase(ctx, "doing the thing");
    pg_expect(ctx, "answer", 42);
    pg_actual(ctx, "answer", 41);   /* off by one — that's the bug */
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "off_by_one",
    .title       = "Off-by-one demo",
    .one_liner   = "expects 42, computes 41",
    .description = "Smallest possible scenario for the tutorial.",
    .expected    = "answer = 42; actual = 41",
    .lesson      =
        "Off-by-one is the second-most-common bug in programming\n"
        "(after naming things). Always test boundary conditions.",
    .category    = PG_CAT_MEMORY,
    .run         = run,
};

PG_SCENARIO_REGISTER(scen);
```

Rebuild and it appears.

## The emit API (event.h)

| Call | What it does |
|---|---|
| `pg_phase(ctx, "name")` | section marker, shown bold/cyan in the log |
| `pg_logf(ctx, fmt, ...)` | free-form log line, attributed to the calling thread |
| `pg_count(ctx, "key", delta)` | counter (accumulated across emits) |
| `pg_gauge(ctx, "key", value)` | gauge, rendered as a `####....` bar in the TUI (auto-scaled to seen min/max) |
| `pg_expect(ctx, "key", value)` | what the user *should* see — appears yellow |
| `pg_actual(ctx, "key", value)` | what they actually get — appears green if matches expect, red if not |
| `pg_sut_fault(ctx, "what")` | scenario-detected failure (the SUT misbehaved) — appears red/bold |
| `pg_done(ctx)` | runner emits this automatically; you don't need to call it |

All of these are safe to call from any thread inside the scenario child; the underlying pipe writes are atomic.

## The chaos primitives

Pull in the headers you need:

```c
#include "playground/chaos_thread.h"   /* spawn, watchdog, cancel-at-time, inject-delay */
#include "playground/chaos_mem.h"      /* mmap'd arena with redzones + UAF poison */
#include "playground/chaos_net.h"      /* socketpair + proxy with loss/delay/fragment */
#include "playground/chaos_io.h"       /* wrapped fd: quota/partial/fsync-delay; clock skew */
```

### chaos_thread (concurrency)
```c
chaos_thread_t *t = chaos_thread_spawn(my_fn, my_arg, "my_tag");
chaos_thread_watchdog(2ULL * 1000000000ULL, "deadlock"); /* arms a 2s kill switch */
chaos_thread_cancel_at_ns(t, 50000000ULL);               /* cancel after 50ms */
chaos_thread_join(t);
```

Tagged-spawn means events from this thread carry a unique `thread_tag` so the TUI can group them.

### chaos_mem (memory)
```c
chaos_arena_t *a = chaos_arena_create(64 * 1024, CHAOS_ARENA_REDZONES);
void *p = chaos_arena_alloc(a, 32);
/* ... do something to p ... */
int corrupted = chaos_arena_check(a);   /* count of corrupted redzones */
chaos_arena_free(a, p);                  /* returns -1 on double-free */
chaos_arena_destroy(a);
```

Redzones are 16-byte poisoned guards before & after each allocation. Free poisons user data with `0xDE` so subsequent UAF reads return obvious garbage. Containment is the arena: corruption never reaches libc's heap or the parent process.

### chaos_net (network)
```c
chaos_net_knobs_t k = {
    .loss_prob       = 0.30,         /* 30% chunk loss */
    .delay_min_ns    = 1000000,
    .delay_max_ns    = 5000000,      /* 1-5ms jitter */
    .max_chunk_bytes = 64,           /* fragment writes */
};
chaos_pair_t pair;
chaos_net_pair(&pair, &k, pg_runctx_seed(ctx));
/* user code uses pair.fd_a and pair.fd_b like normal stream sockets */
chaos_net_close(&pair);
```

The proxy is two threads sitting between the user-visible fds, applying chaos. The user calls real `read`/`write`/`select`/`poll`.

### chaos_io (time + I/O)
```c
int fd = chaos_io_open("/scratch", O_WRONLY);
chaos_io_set_quota      (fd, 1024);                    /* ENOSPC after 1 KiB */
chaos_io_set_partial    (fd, 137);                     /* short writes/reads */
chaos_io_set_fsync_delay(fd, 1500ULL * 1000000ULL);    /* fsync blocks 1.5s */

ssize_t w = chaos_io_write(fd, buf, n);
chaos_io_close(fd);

chaos_clock_skew_set  (-30LL * 1000000000LL);   /* clock jumps -30s */
chaos_clock_jitter_set(500000ULL);              /* up to 500us jitter */
uint64_t now = chaos_clock_now_ns();
```

Backed by real fds (mkstemp + auto-unlink), so `read`/`write`/`fsync` semantics are real — only the policy (quota, short reads, delay) is synthetic.

## Writing a good lesson

The `lesson` field is shown in the explain overlay (press `e` after a run). This is where the educational payoff lives.

What works:

- **Lead with *why* the bug happens**, not just *what*. The reader can see *what* in the event log.
- **Name the fixes concretely** (`pthread_mutex`, `atomic_fetch_add`, `signal(SIGPIPE, SIG_IGN)`) — readers should leave with actions.
- **Mention the relevant tool**: AddressSanitizer for UAF, ThreadSanitizer for races, etc.
- **Keep it under ~20 lines**; the overlay wraps but doesn't scroll.

Look at any existing scenario for examples (e.g. `src/scenarios/memory/uaf_callback.c`).

## Tips & gotchas

- **Use the seed** (`pg_runctx_seed(ctx)`, `pg_runctx_rand(ctx)`) for any randomness you want reproducible across runs. Real OS races are intentionally non-deterministic.
- **Use `chaos_thread_watchdog`** for any scenario that *might* hang (deadlocks, infinite waits). Default is to hard-`_exit(2)` after the timeout; the parent reads the fault event and reaps cleanly.
- **Don't trash the child's heap** — keep memory chaos inside `chaos_arena`. The parent never touches scenario memory (different process), but the child still needs to walk its own structures cleanly until exit.
- **Ignore `SIGPIPE`** at the start of any network scenario that might write to a closed peer: `signal(SIGPIPE, SIG_IGN)`. Otherwise the process dies before `write()` returns.
- **Real OS-level chaos is out of scope** — no `tc`, no `pfctl`, no signals to other PIDs. The point of in-process simulation is no sudo, no platform-specific privilege flags.
- **Don't call `pg_done`** — the runner emits it automatically after `run()` and `teardown()` complete.
- **Tagged threads**: events from a thread spawned via `chaos_thread_spawn(fn, arg, "tag")` are tagged with a unique id so the TUI can group them. Plain `pthread_create` threads emit untagged events (thread_tag = 0).

## Verifying

```bash
cmake --build build && ./build/playground_run --scenario <id>          # text output
cmake --build build && ./build/playground_run --scenario <id> --json   # JSONL
cmake --build build && ./build/playground                              # interactive
```

For sanitizer coverage:
```bash
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan && cmake --build build-tsan
./build-tsan/playground_run --scenario <id>
```
