#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Open a real temporary file (the virtpath is informational, used only to
 * disambiguate paths in user-visible logs). Returns a real fd you can pass
 * to the chaos_io_* helpers below, or to plain syscalls if you want
 * unmolested behavior. The file is unlinked immediately so it disappears
 * when closed. Always opened O_RDWR + O_CREAT under the hood. */
int     chaos_io_open (const char *virtpath);

/* Wrapped read/write/fsync/close honoring the per-fd knobs set below. */
ssize_t chaos_io_write(int fd, const void *buf, size_t n);
ssize_t chaos_io_read (int fd, void *buf, size_t n);
int     chaos_io_fsync(int fd);
int     chaos_io_close(int fd);

/* After this many bytes have been written through chaos_io_write, subsequent
 * writes return -1 with errno=ENOSPC. 0 disables the quota. */
void    chaos_io_set_quota       (int fd, size_t bytes_until_enospc);

/* Truncate writes/reads to at most this many bytes per call. 0 disables. */
void    chaos_io_set_partial     (int fd, size_t max_chunk);

/* Make chaos_io_fsync block for this many ns before calling real fsync. */
void    chaos_io_set_fsync_delay (int fd, uint64_t ns);

/* Process-global clock helpers. chaos_clock_now_ns() returns the real
 * CLOCK_MONOTONIC plus skew plus uniform jitter in [0, jitter_max). */
uint64_t chaos_clock_now_ns       (void);
void     chaos_clock_skew_set     (int64_t  offset_ns);
void     chaos_clock_jitter_set   (uint64_t max_ns);

/* Seed the jitter RNG. Called by the runner before each scenario so that the
 * jitter pattern is reproducible across runs with the same scenario seed. */
void     chaos_clock_seed         (uint64_t seed);
