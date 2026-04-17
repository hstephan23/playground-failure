#pragma once

#include "scenario.h"
#include <stdint.h>

typedef struct chaos_thread chaos_thread_t;

/* Spawn a thread tagged for event-stream attribution. The tag string is
 * informational; events emitted from this thread will carry a unique
 * thread_tag id so the TUI can group them. Returns NULL on failure. */
chaos_thread_t *chaos_thread_spawn(void *(*fn)(void *), void *arg, const char *tag);

/* Wait for the thread to exit and free the handle. The handle is invalid
 * after this returns — do not call any other chaos_thread_* on it. */
int chaos_thread_join(chaos_thread_t *t);

/* Set the cooperative stop flag and pthread_cancel the thread. Both signals
 * fire: pthread_cancel handles I/O-bound or sleeping threads via cancellation
 * points; the flag handles compute-bound threads that poll. Returns 0/-1. */
int chaos_thread_cancel(chaos_thread_t *t);

/* Set the stop flag + pthread_cancel after `after_ns` from now. The caller
 * must NOT join the thread before `after_ns` elapses, or the timer thread
 * may dereference a freed handle. */
int chaos_thread_cancel_at_ns(chaos_thread_t *t, uint64_t after_ns);

/* Read the stop flag. Threads in tight compute loops should poll this and
 * exit when it returns non-zero. Threads doing blocking I/O / sleeps will be
 * unblocked by pthread_cancel and don't need to poll. */
int chaos_thread_should_stop(chaos_thread_t *t);

/* Sleep the calling thread for a uniformly-random delay in [min_ns, max_ns],
 * drawn from ctx's seeded RNG. */
int chaos_thread_inject_delay(pg_runctx_t *ctx, uint64_t min_ns, uint64_t max_ns);

/* Arm a one-shot watchdog. If `timeout_ns` elapses before the scenario exits,
 * emits PG_EV_FAULT("watchdog: <what>") and calls _exit(2). The parent process
 * sees the fault event and the abnormal exit. Use this for scenarios that
 * may hang (deadlocks). The watchdog dies with the process if the scenario
 * completes first — no explicit disarm is needed. */
void chaos_thread_watchdog(uint64_t timeout_ns, const char *what);
