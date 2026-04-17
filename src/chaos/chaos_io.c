#include "playground/chaos_io.h"
#include "core/rand.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int       fd;
    int       in_use;
    size_t    quota;
    size_t    bytes_written;
    size_t    partial_max;
    uint64_t  fsync_delay_ns;
    char      path[256];
} io_slot_t;

#define IO_SLOTS_MAX 64

static io_slot_t        g_slots[IO_SLOTS_MAX];
static pthread_mutex_t  g_slots_mu = PTHREAD_MUTEX_INITIALIZER;

static io_slot_t *find_locked(int fd) {
    for (int i = 0; i < IO_SLOTS_MAX; ++i) {
        if (g_slots[i].in_use && g_slots[i].fd == fd) return &g_slots[i];
    }
    return NULL;
}

static io_slot_t *alloc_locked(int fd) {
    for (int i = 0; i < IO_SLOTS_MAX; ++i) {
        if (!g_slots[i].in_use) {
            memset(&g_slots[i], 0, sizeof(g_slots[i]));
            g_slots[i].in_use = 1;
            g_slots[i].fd     = fd;
            return &g_slots[i];
        }
    }
    return NULL;
}

int chaos_io_open(const char *virtpath, int flags) {
    (void)virtpath;
    (void)flags;
    char tmpl[] = "/tmp/playground.XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    /* Unlink so the file vanishes when closed — keeps /tmp clean across runs. */
    unlink(tmpl);

    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = alloc_locked(fd);
    if (!s) {
        pthread_mutex_unlock(&g_slots_mu);
        close(fd);
        errno = EMFILE;
        return -1;
    }
    snprintf(s->path, sizeof(s->path), "%s", tmpl);
    pthread_mutex_unlock(&g_slots_mu);
    return fd;
}

ssize_t chaos_io_write(int fd, const void *buf, size_t n) {
    size_t want = n;
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    if (s) {
        if (s->quota > 0) {
            if (s->bytes_written >= s->quota) {
                pthread_mutex_unlock(&g_slots_mu);
                errno = ENOSPC;
                return -1;
            }
            size_t rem = s->quota - s->bytes_written;
            if (want > rem) want = rem;
        }
        if (s->partial_max > 0 && want > s->partial_max) want = s->partial_max;
    }
    pthread_mutex_unlock(&g_slots_mu);

    ssize_t w = write(fd, buf, want);
    if (w > 0) {
        pthread_mutex_lock(&g_slots_mu);
        io_slot_t *s2 = find_locked(fd);
        if (s2) s2->bytes_written += (size_t)w;
        pthread_mutex_unlock(&g_slots_mu);
    }
    return w;
}

ssize_t chaos_io_read(int fd, void *buf, size_t n) {
    size_t want = n;
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    if (s && s->partial_max > 0 && want > s->partial_max) want = s->partial_max;
    pthread_mutex_unlock(&g_slots_mu);
    return read(fd, buf, want);
}

int chaos_io_fsync(int fd) {
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    uint64_t delay = s ? s->fsync_delay_ns : 0;
    pthread_mutex_unlock(&g_slots_mu);

    if (delay > 0) {
        struct timespec ts = {
            .tv_sec  = (time_t)(delay / 1000000000ULL),
            .tv_nsec = (long)  (delay % 1000000000ULL),
        };
        nanosleep(&ts, NULL);
    }
    return fsync(fd);
}

int chaos_io_close(int fd) {
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    if (s) s->in_use = 0;
    pthread_mutex_unlock(&g_slots_mu);
    return close(fd);
}

void chaos_io_set_quota(int fd, size_t bytes) {
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    if (s) s->quota = bytes;
    pthread_mutex_unlock(&g_slots_mu);
}

void chaos_io_set_partial(int fd, size_t max_chunk) {
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    if (s) s->partial_max = max_chunk;
    pthread_mutex_unlock(&g_slots_mu);
}

void chaos_io_set_fsync_delay(int fd, uint64_t ns) {
    pthread_mutex_lock(&g_slots_mu);
    io_slot_t *s = find_locked(fd);
    if (s) s->fsync_delay_ns = ns;
    pthread_mutex_unlock(&g_slots_mu);
}

/* ------------------------- clock helpers ------------------------- */

static int64_t  g_clock_skew_ns       = 0;
static uint64_t g_clock_jitter_max_ns = 0;
static pg_xosh_t g_clock_rng = { .s = { 1, 2, 3, 4 } };
static pthread_mutex_t g_clock_mu = PTHREAD_MUTEX_INITIALIZER;

uint64_t chaos_clock_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t base = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;

    pthread_mutex_lock(&g_clock_mu);
    base += g_clock_skew_ns;
    if (g_clock_jitter_max_ns > 0) {
        int64_t j = (int64_t)(pg_xosh_next(&g_clock_rng) % g_clock_jitter_max_ns);
        base += j;
    }
    pthread_mutex_unlock(&g_clock_mu);

    return (uint64_t)base;
}

void chaos_clock_skew_set(int64_t offset_ns) {
    pthread_mutex_lock(&g_clock_mu);
    g_clock_skew_ns = offset_ns;
    pthread_mutex_unlock(&g_clock_mu);
}

void chaos_clock_jitter_set(uint64_t max_ns) {
    pthread_mutex_lock(&g_clock_mu);
    g_clock_jitter_max_ns = max_ns;
    pthread_mutex_unlock(&g_clock_mu);
}
