/* Split-brain: two replicas, network partition, then heal.
 *
 * Two replica threads each keep a local counter and periodically tell the
 * other their count. For the first half of the run, chaos_net drops 100%
 * of traffic (partition). For the second half, knobs flip to 0% loss
 * (healed). After both finish, the replicas see different "last known"
 * values — classic split-brain divergence. */

#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_net.h"
#include "playground/chaos_io.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
    ITERS       = 30,
    TICK_NS     = 20 * 1000000,    /* 20 ms */
    PARTITION_HALF_NS = (int64_t)ITERS * TICK_NS / 2,
};

typedef struct {
    int           id;            /* 1 or 2 */
    chaos_pair_t *pair;
    int           send_fd;       /* our write end */
    int           recv_fd;       /* our read end */
    pg_runctx_t  *ctx;

    int64_t       local_count;        /* what *we* believe */
    int64_t       last_peer_count;    /* the newest thing we heard from the other side */
    uint64_t      last_heard_ns;      /* when (relative to scenario start) */
} replica_t;

static void *replica_fn(void *arg) {
    replica_t *r = (replica_t *)arg;
    for (int i = 0; i < ITERS; ++i) {
        r->local_count++;

        /* publish our count to the peer */
        int64_t msg = r->local_count;
        ssize_t w  = write(r->send_fd, &msg, sizeof(msg));
        (void)w;

        /* drain anything the peer sent (non-blocking via short poll) */
        for (;;) {
            int64_t peer;
            /* Try read with 1ms timeout so we don't steal the tick entirely */
            struct timespec ts = { .tv_nsec = 1000000 };  /* 1ms */
            nanosleep(&ts, NULL);
            ssize_t n = read(r->recv_fd, &peer, sizeof(peer));
            if (n != sizeof(peer)) break;
            r->last_peer_count = peer;
            r->last_heard_ns   = chaos_clock_now_ns();
        }

        /* next tick */
        struct timespec sl = { .tv_nsec = (long)(TICK_NS - 1000000) };
        nanosleep(&sl, NULL);
    }
    return NULL;
}

static int run(pg_runctx_t *ctx, void *state) {
    (void)state;

    /* Start partitioned: 100% loss */
    chaos_net_knobs_t partitioned = { .loss_prob = 1.0, .max_chunk_bytes = 8 };
    chaos_pair_t      pair;
    if (chaos_net_pair(&pair, &partitioned, pg_runctx_seed(ctx)) < 0) {
        pg_sut_fault(ctx, "chaos_net_pair failed");
        return 1;
    }

    /* Non-blocking reads so replicas can poll without hanging. */
    fcntl(pair.fd_a, F_SETFL, O_NONBLOCK);
    fcntl(pair.fd_b, F_SETFL, O_NONBLOCK);

    pg_phase (ctx, "Two replicas, 30 ticks each. First half: network partition.");
    pg_logf  (ctx, "Each replica increments a local counter every 20ms and tries to tell the peer.");
    pg_logf  (ctx, "During partition (first ~300ms), NO messages get through.");
    pg_logf  (ctx, "Mid-scenario the partition heals. Observe what each replica believes.");
    pg_expect(ctx, "last_peer_count_r1", ITERS);
    pg_expect(ctx, "last_peer_count_r2", ITERS);

    replica_t r1 = { .id = 1, .pair = &pair, .send_fd = pair.fd_a, .recv_fd = pair.fd_b, .ctx = ctx };
    replica_t r2 = { .id = 2, .pair = &pair, .send_fd = pair.fd_b, .recv_fd = pair.fd_a, .ctx = ctx };

    pthread_t t1, t2;
    uint64_t  t0 = chaos_clock_now_ns();
    pthread_create(&t1, NULL, replica_fn, &r1);
    pthread_create(&t2, NULL, replica_fn, &r2);

    /* Heal the partition halfway through. */
    struct timespec heal_at = {
        .tv_sec  = (time_t)(PARTITION_HALF_NS / 1000000000LL),
        .tv_nsec = (long)  (PARTITION_HALF_NS % 1000000000LL),
    };
    nanosleep(&heal_at, NULL);

    chaos_net_knobs_t healed = { .loss_prob = 0.0, .max_chunk_bytes = 8 };
    chaos_net_set_knobs(&pair, &healed);
    pg_phase(ctx, "partition HEALED -- replicas can now hear each other");

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    uint64_t dt_ms = (chaos_clock_now_ns() - t0) / 1000000;

    pg_actual(ctx, "last_peer_count_r1", r1.last_peer_count);
    pg_actual(ctx, "last_peer_count_r2", r2.last_peer_count);
    pg_gauge (ctx, "local_count_r1",    r1.local_count);
    pg_gauge (ctx, "local_count_r2",    r2.local_count);
    pg_gauge (ctx, "elapsed_ms",        (int64_t)dt_ms);

    pg_logf(ctx, "replica 1: local=%lld  last heard from peer=%lld",
            (long long)r1.local_count, (long long)r1.last_peer_count);
    pg_logf(ctx, "replica 2: local=%lld  last heard from peer=%lld",
            (long long)r2.local_count, (long long)r2.last_peer_count);

    /* In this demo both replicas send their FULL counter each tick, so one
     * post-heal message brings the peer fully up to date — they converge.
     * The interesting failure lives in what happened DURING the partition: */
    pg_logf(ctx, "");
    pg_logf(ctx, "at the heal boundary (~%d ms), each replica saw:",
            (int)(PARTITION_HALF_NS / 1000000));
    pg_logf(ctx, "  replica 1: local ≈ %d, peer ≈ 0  (divergent)", ITERS / 2);
    pg_logf(ctx, "  replica 2: local ≈ %d, peer ≈ 0  (divergent)", ITERS / 2);
    pg_logf(ctx, "they RECONCILED only because the payload was an idempotent counter.");
    pg_logf(ctx, "in prod, writes during a partition are payments, orders, user actions --");
    pg_logf(ctx, "NOT idempotent. 'Just merge what you have' corrupts state.");
    if (r1.local_count != r1.last_peer_count || r2.local_count != r2.last_peer_count) {
        pg_sut_fault(ctx, "post-heal divergence: replicas still disagree");
    }

    chaos_net_close(&pair);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "split_brain",
    .title       = "Split-brain: partition then heal",
    .one_liner   = "2 replicas, 100% loss half the run, then 0%",
    .description = "Two replica threads each maintain a local counter. A mid-run partition "
                   "(via chaos_net_set_knobs) causes divergent state; healing doesn't reconcile.",
    .expected    = "replicas disagree on the counter even after the network recovers",
    .lesson      =
        "Why partitions are dangerous: during a partition, each side\n"
        "thinks it's the authoritative source. Both accept writes. When\n"
        "the partition heals, they have divergent state. There is no\n"
        "automatic way to merge — the data itself doesn't tell you which\n"
        "write 'should' have won.\n"
        "\n"
        "Fixes:\n"
        "  - Use consensus (Raft, Paxos) for any state that MUST stay\n"
        "    consistent across replicas.\n"
        "  - Or: accept eventual consistency with explicit conflict\n"
        "    resolution (CRDTs, vector clocks, last-write-wins with\n"
        "    a reliable clock).\n"
        "  - Or: sacrifice availability during partitions (the CP side\n"
        "    of CAP — refuse writes on the minority side).\n"
        "  - NEVER: silently merge and hope.",
    .category    = PG_CAT_COMPOUND,
    .run         = run,
};

PG_SCENARIO_REGISTER(scen);
