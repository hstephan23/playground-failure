#pragma once

#include "scenario.h"
#include "event.h"
#include <sys/types.h>
#include <stdint.h>

typedef struct {
    pid_t    child_pid;
    int      event_fd;     /* read end of event pipe (parent side) */
    int      ctrl_fd;      /* write end of control pipe (parent side) — reserved */
    uint64_t seed;
} pg_run_handle_t;

/* Fork a child, run the scenario inside it, return a handle whose event_fd
 * yields fixed-size pg_event_t records. Returns 0 on success. */
int  pg_run_start(const pg_scenario_t *s, uint64_t seed, pg_run_handle_t *out);

/* Read one event. Returns 1 = got an event, 0 = EOF (child closed pipe),
 * -1 = error or partial read. timeout_ms is currently advisory. */
int  pg_run_poll (pg_run_handle_t *h, pg_event_t *ev_out, int timeout_ms);

/* Send SIGTERM (then SIGKILL if it lingers). Best-effort. */
void pg_run_abort(pg_run_handle_t *h);

/* Wait for child to exit, close the event_fd. */
int  pg_run_reap (pg_run_handle_t *h, int *exit_status);
