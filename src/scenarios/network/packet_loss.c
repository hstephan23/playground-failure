#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_net.h"

#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int pl_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    chaos_net_knobs_t k = {
        .loss_prob       = 0.30,
        .max_chunk_bytes = 16,    /* fragment so loss happens at message granularity */
    };
    chaos_pair_t pair;
    if (chaos_net_pair(&pair, &k, pg_runctx_seed(ctx)) < 0) {
        pg_sut_fault(ctx, "chaos_net_pair failed");
        return 1;
    }

    enum { N = 100, MSG = 16 };
    pg_phase (ctx, "sending 100 x 16-byte messages with 30% chunk loss");
    pg_expect(ctx, "bytes_received", (int64_t)N * MSG);

    char msg[MSG];
    memset(msg, 'X', MSG);
    int64_t sent = 0;
    for (int i = 0; i < N; ++i) {
        ssize_t w = write(pair.fd_a, msg, MSG);
        if (w > 0) sent += w;
    }
    pg_count(ctx, "bytes_sent", sent);

    pg_phase(ctx, "receiving (200ms idle timeout per read)");
    int64_t        received = 0;
    struct pollfd  pf       = { .fd = pair.fd_b, .events = POLLIN };
    char           buf[256];
    for (;;) {
        int pr = poll(&pf, 1, 200);
        if (pr <= 0) break;
        ssize_t r = read(pair.fd_b, buf, sizeof(buf));
        if (r <= 0) break;
        received += r;
    }
    pg_actual(ctx, "bytes_received", received);
    pg_logf(ctx, "lost %lld of %lld bytes (%.1f%%)",
            (long long)(sent - received), (long long)sent,
            sent > 0 ? 100.0 * (double)(sent - received) / (double)sent : 0.0);
    pg_logf(ctx, "with TCP this NEVER happens — the kernel retransmits.");
    pg_logf(ctx, "with UDP, this is exactly your day-to-day reality.");

    chaos_net_close(&pair);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "packet_loss",
    .title       = "Network: 30% packet loss",
    .one_liner   = "100 msgs of 16B, proxy drops chunks",
    .description = "Sends 100 messages through a chaos proxy that drops 30% of chunks. Demonstrates byte-level loss in a stream.",
    .expected    = "received < sent; ~30% of bytes vanish",
    .lesson      =
        "Why TCP doesn't show this: the kernel retransmits dropped segments\n"
        "invisibly. You never see the loss as an application. With UDP, the\n"
        "loss is your reality -- you must build retries, sequence numbers,\n"
        "and timeouts on top of it.\n"
        "\n"
        "Fixes / context:\n"
        "  - Use TCP unless you have a reason not to.\n"
        "  - Build a reliability layer (sequence + ack + retry) on UDP.\n"
        "  - QUIC packages all of that for you (HTTP/3 runs on it).\n"
        "  - For real-time work where retransmits are too slow (audio,\n"
        "    games), accept loss and design around it (FEC, interpolation).",
    .category    = PG_CAT_NETWORK,
    .run         = pl_run,
};

PG_SCENARIO_REGISTER(scen);
