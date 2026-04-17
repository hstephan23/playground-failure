#pragma once

/* The wire format scenarios use to talk to the parent process.
 *
 * Each event is a fixed-size 256-byte record so a partial pipe write from
 * a crashing child can be re-synced at the next 256-byte boundary.
 * pg_emit and friends are safe to call from any thread inside the scenario
 * child — the underlying write(2) is atomic for sizes <= PIPE_BUF (>= 512
 * on macOS/Linux). */

#include <stdint.h>
#include <stddef.h>
#include "scenario.h"

typedef enum {
    PG_EV_LOG         = 0,
    PG_EV_COUNTER     = 1,
    PG_EV_GAUGE       = 2,
    PG_EV_PHASE       = 3,
    PG_EV_EXPECT      = 4,
    PG_EV_ACTUAL      = 5,
    PG_EV_SUT_FAULT   = 6,    /* the system-under-test misbehaved (the
                                  scenario's intentional failure) */
    PG_EV_DONE        = 7,
    PG_EV_CHILD_CRASH = 8,    /* child process died from a signal — emitted
                                  by the runner's signal handler */
    PG_EV_WATCHDOG    = 9     /* harness-detected hang — emitted by
                                  chaos_thread_watchdog */
} pg_event_kind_t;

const char *pg_event_kind_name(pg_event_kind_t k);

#define PG_EVENT_KEY_LEN  32
#define PG_EVENT_TEXT_LEN 192

#define PG_EVENT_MAGIC   ((uint16_t)0xE5E7)
#define PG_EVENT_VERSION ((uint8_t)1)

/* Fixed-size event record. Total size is 256 bytes so a partial pipe write
 * from a crashing child can be resync'd at the next 256-byte boundary.
 * The 4-byte header (magic/version/pad) lets the parent detect misalignment
 * after a crash and reject corrupted records loudly instead of silently
 * decoding garbage. */
typedef struct {
    uint16_t magic;         /* PG_EVENT_MAGIC */
    uint8_t  version;       /* PG_EVENT_VERSION */
    uint8_t  _pad0;
    uint32_t kind;          /* pg_event_kind_t */
    uint32_t thread_tag;
    /* 4 bytes implicit padding here (alignment of ts_ns) — fills the slot
     * the v0 layout used as part of _reserved[8]. */
    uint64_t ts_ns;
    int64_t  num;
    char     key [PG_EVENT_KEY_LEN];     /* 32 */
    char     text[PG_EVENT_TEXT_LEN];    /* 192 */
} pg_event_t;

_Static_assert(sizeof(pg_event_t) == 256, "pg_event_t must be exactly 256 bytes");

/* Scenario-side emit API. Safe from any thread; the underlying pipe write
 * is atomic for sizeof(pg_event_t) (well under PIPE_BUF on macOS/Linux). */
void pg_emit     (pg_runctx_t *ctx, const pg_event_t *ev);
void pg_logf     (pg_runctx_t *ctx, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pg_phase    (pg_runctx_t *ctx, const char *name);
void pg_count    (pg_runctx_t *ctx, const char *key, int64_t delta);
void pg_gauge    (pg_runctx_t *ctx, const char *key, int64_t value);
void pg_expect   (pg_runctx_t *ctx, const char *key, int64_t value);
void pg_actual   (pg_runctx_t *ctx, const char *key, int64_t value);
/* The system-under-test misbehaved in the way the scenario was demonstrating. */
void pg_sut_fault(pg_runctx_t *ctx, const char *what);
/* Harness-detected hang. Used internally by chaos_thread_watchdog. */
void pg_watchdog (pg_runctx_t *ctx, const char *what);
void pg_done     (pg_runctx_t *ctx);
