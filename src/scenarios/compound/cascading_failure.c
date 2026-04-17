/* Cascading failure: lossy network → slow-fsync receiver.
 *
 * A sender thread pushes N chunks through a chaos_net pair with 20% loss.
 * A receiver thread pulls bytes out and persists them via chaos_io_write +
 * chaos_io_fsync (50ms fsync delay). Individually each failure is easy to
 * tolerate; together they gate total throughput on tail latency. */

#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_net.h"
#include "playground/chaos_io.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

enum {
    CHUNKS       = 50,
    CHUNK_BYTES  = 256,
    FSYNC_MS     = 50,
    FSYNC_DELAY  = (int64_t)FSYNC_MS * 1000000,  /* ns */
};

typedef struct {
    chaos_pair_t *pair;
    int           io_fd;
    pg_runctx_t  *ctx;
    int64_t       bytes_received;
    int64_t       bytes_persisted;
    int64_t       fsync_count;
} receiver_state_t;

static void *receiver_fn(void *arg) {
    receiver_state_t *s = (receiver_state_t *)arg;
    char              buf[4096];
    for (;;) {
        ssize_t n = read(s->pair->fd_b, buf, sizeof(buf));
        if (n <= 0) break;
        s->bytes_received += n;

        ssize_t w = chaos_io_write(s->io_fd, buf, (size_t)n);
        if (w > 0) s->bytes_persisted += w;
        chaos_io_fsync(s->io_fd);
        s->fsync_count++;
    }
    return NULL;
}

static int run(pg_runctx_t *ctx, void *state) {
    (void)state;

    /* 20% packet loss, 64-byte chunks so loss happens at fine granularity */
    chaos_net_knobs_t net_k = { .loss_prob = 0.20, .max_chunk_bytes = 64 };
    chaos_pair_t      pair;
    if (chaos_net_pair(&pair, &net_k, pg_runctx_seed(ctx)) < 0) {
        pg_sut_fault(ctx, "chaos_net_pair failed");
        return 1;
    }

    int io_fd = chaos_io_open("/tmp/cascade");
    if (io_fd < 0) {
        pg_sut_fault(ctx, "chaos_io_open failed");
        chaos_net_close(&pair);
        return 1;
    }
    chaos_io_set_fsync_delay(io_fd, FSYNC_DELAY);

    pg_phase (ctx, "sending through 20% loss → persisting with 50ms fsync");
    pg_expect(ctx, "bytes_persisted", (int64_t)CHUNKS * CHUNK_BYTES);

    receiver_state_t s = { .pair = &pair, .io_fd = io_fd, .ctx = ctx };
    pthread_t        rt;
    pthread_create(&rt, NULL, receiver_fn, &s);

    uint64_t t0 = chaos_clock_now_ns();
    char     chunk[CHUNK_BYTES];
    memset(chunk, 'C', sizeof(chunk));
    int64_t bytes_sent = 0;
    for (int i = 0; i < CHUNKS; ++i) {
        ssize_t w = write(pair.fd_a, chunk, sizeof(chunk));
        if (w > 0) bytes_sent += w;
    }
    /* close sender side so receiver sees EOF after drain */
    close(pair.fd_a);
    pair.fd_a = -1;

    pthread_join(rt, NULL);
    uint64_t dt = chaos_clock_now_ns() - t0;
    int64_t  ms = (int64_t)(dt / 1000000);

    pg_count (ctx, "bytes_sent",      bytes_sent);
    pg_count (ctx, "bytes_received",  s.bytes_received);
    pg_actual(ctx, "bytes_persisted", s.bytes_persisted);
    pg_count (ctx, "fsync_count",     s.fsync_count);
    pg_gauge (ctx, "elapsed_ms",      ms);

    int64_t lost      = bytes_sent - s.bytes_received;
    int64_t expected  = (int64_t)CHUNKS * CHUNK_BYTES;
    int64_t lost_pct  = expected > 0 ? (100 * lost) / expected : 0;
    pg_logf(ctx, "sent=%lld received=%lld persisted=%lld  (lost %lld B = %lld%%)",
            (long long)bytes_sent, (long long)s.bytes_received,
            (long long)s.bytes_persisted, (long long)lost, (long long)lost_pct);
    pg_logf(ctx, "elapsed=%lld ms across %lld fsyncs (≈ %lld ms/fsync)",
            (long long)ms, (long long)s.fsync_count,
            s.fsync_count ? (long long)(ms / s.fsync_count) : 0);

    chaos_io_close(io_fd);
    chaos_net_close(&pair);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "cascading_failure",
    .title       = "Cascade: loss + slow fsync",
    .one_liner   = "20% net loss, 50ms fsyncs, concurrent receiver",
    .description = "A lossy network feeds a receiver that persists each chunk via slow fsync. "
                   "Individually each primitive is tolerable; compounded they tank throughput.",
    .expected    = "bytes_persisted < bytes_sent (some lost), wall-time >> single-primitive case",
    .lesson      =
        "Why compounding failures are worse than they look:\n"
        "\n"
        "Individually, each chaos source is routine:\n"
        "  - 20% packet loss → TCP retransmits invisibly.\n"
        "  - 50ms fsync      → ~20 ops/sec per thread.\n"
        "  - 1 receiver thread → naive serial processing.\n"
        "\n"
        "Combined they compound:\n"
        "  - Lost chunks lengthen the tail (retries not modeled here;\n"
        "    in prod they'd arrive out of order too).\n"
        "  - Every fsync serializes the receiver; when the sender\n"
        "    sprints, the queue grows and tail latency follows.\n"
        "  - If the sender had a timeout, it would start firing.\n"
        "\n"
        "Fixes:\n"
        "  - Batch writes between fsyncs (group commit).\n"
        "  - Separate thread pools per stage with explicit queues.\n"
        "  - Apply backpressure BEFORE the timeout tier.\n"
        "  - Test the compound scenario; individual-component SLOs\n"
        "    never predict compound-failure behavior.",
    .category    = PG_CAT_COMPOUND,
    .run         = run,
};

PG_SCENARIO_REGISTER(scen);
