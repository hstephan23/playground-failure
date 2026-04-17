# Failure Playground

An interactive C/TUI tool for breaking systems on purpose, so you can build intuition for what failure actually looks like from the inside.

Most dev tools help you succeed. This one is the opposite: every scenario intentionally breaks something — a thread, a buffer, a socket, a clock — and shows you the failure live, in a 3-pane TUI with event log, counters, and an expected-vs-actual diff.

```
┌─ Failure Playground ─────────────────────────────────────────────┐
│  Race: lost increments     │ EVENT LOG                           │
│  Deadlock: opposite order  │ 0.001 -- spawning writers --        │
│  Cancel mid-write          │ 0.002 log[t1] spawned writer 1      │
│  UAF: dangling closure     │ 0.012 expect counter = 8000         │
│  Double-free detected      │ 2.301 actual counter = 1960         │
│  ... 15 scenarios total    │                                     │
│                            │ STATS                               │
│  alloc, free, then read    │  EXPECTED vs ACTUAL                 │
│  expect: 0xDEDE garbage    │   counter   8000 vs 1960  (-6040)   │
├──────────────────────────────────────────────────────────────────┤
│ [r] rerun  [R] new seed  [e] lesson  [ENTER] menu  [q] quit      │
└──────────────────────────────────────────────────────────────────┘
```

Why C? Real pthreads, real OOB writes, real signals — the failures are visceral. Each scenario runs in a forked child, so even a SIGSEGV stays inside the sandbox; the TUI keeps running.

## Build

Requires CMake ≥ 3.20, clang or gcc, ncurses (system on macOS, `brew install ncurses` if missing), and pthreads.

```bash
cmake -B build && cmake --build build
```

Sanitizer builds (catch the bugs scenarios are demonstrating):

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Asan && cmake --build build-asan
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Tsan && cmake --build build-tsan
```

## Run

Interactive TUI (recommended for the live experience):
```bash
./build/playground
```
Use `↑`/`↓` to pick a scenario, `Enter` to run it, `e` to read the lesson overlay, `r`/`R` to rerun (same seed / new seed), `q` to quit.

Headless (good for scripting, CI, capturing event streams):
```bash
./build/playground_run --list
./build/playground_run --scenario race_counter --seed 42
./build/playground_run --scenario race_counter --seed 42 --json
```

## What's in the box (15 scenarios)

| Category | Scenarios |
|---|---|
| **concurrency** | `race_counter`, `deadlock_classic`, `thread_cancel_midwrite` |
| **memory** | `uaf_callback`, `double_free`, `arena_corrupt`, `crash_null_deref` |
| **network** | `packet_loss`, `partial_read`, `connection_reset` |
| **time + I/O** | `disk_full`, `slow_fsync`, `partial_write`, `clock_jump`, `jittery_timer` |

Each scenario emits an event stream the TUI visualizes; each ships with a multi-paragraph "lesson" explaining *why* the failure happens and how to defend against it.

## Project layout

```
include/playground/   public headers (the plug-in contract)
src/main.c            arg parsing, dispatches to TUI or headless mode
src/core/             registry, fork-runner, IPC pipe, seeded RNG
src/tui/              ncurses interface
src/chaos/            primitive subsystems: thread, mem, net, io
src/scenarios/        one .c per scenario, auto-discovered by CMake
scripts/              scaffold_scenario.sh (stamps out a new scenario)
docs/                 architecture and scenario-authoring guides
```

## Adding your own scenarios

```bash
./scripts/scaffold_scenario.sh memory my_idea
# fill in the TODOs in src/scenarios/memory/my_idea.c
cmake --build build
./build/playground            # your scenario appears in the menu
```

See [`docs/ADDING_A_SCENARIO.md`](docs/ADDING_A_SCENARIO.md) for the full tutorial and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design rationale.

## Platform

Primary target is macOS arm64 (Darwin). Linux is a secondary target (the kqueue/timer_create split is the only divergence). Windows is out of scope — too much of the design depends on `fork`, POSIX signals, and `pthread_cancel`.
