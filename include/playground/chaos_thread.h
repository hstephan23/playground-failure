#pragma once

#include "scenario.h"
#include <stdint.h>

typedef struct chaos_thread chaos_thread_t;

/* Spawn a thread tagged for event-stream attribution. The tag string is
 * informational; events emitted from this thread will carry a unique
 * thread_tag id so the TUI can group them. Returns NULL on failure. */
chaos_thread_t *chaos_thread_spawn(void *(*fn)(void *), void *arg, const char *tag);

/* Wait for the thread to exit and free the handle. Returns 0/-1. */
int chaos_thread_join(chaos_thread_t *t);

/* pthread_cancel the thread. Returns 0/-1. */
int chaos_thread_cancel(chaos_thread_t *t);

/* Schedule a pthread_cancel for `t` after `after_ns` from now. */
int chaos_thread_cancel_at_ns(chaos_thread_t *t, uint64_t after_ns);

/* Sleep the calling thread for a uniformly-random delay in [min_ns, max_ns],
 * drawn from ctx's seeded RNG. */
int chaos_thread_inject_delay(pg_runctx_t *ctx, uint64_t min_ns, uint64_t max_ns);

/* Arm a one-shot watchdog. If `timeout_ns` elapses before the scenario exits,
 * emits PG_EV_FAULT("watchdog: <what>") and calls _exit(2). The parent process
 * sees the fault event and the abnormal exit. Use this for scenarios that
 * may hang (deadlocks). The watchdog dies with the process if the scenario
 * completes first — no explicit disarm is needed. */
void chaos_thread_watchdog(uint64_t timeout_ns, const char *what);
