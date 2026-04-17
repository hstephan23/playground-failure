#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_net.h"

#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int pr_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    chaos_net_knobs_t k = { .max_chunk_bytes = 17 };  /* fragment but no loss */
    chaos_pair_t pair;
    if (chaos_net_pair(&pair, &k, pg_runctx_seed(ctx)) < 0) {
        pg_sut_fault(ctx, "chaos_net_pair failed");
        return 1;
    }

    enum { TOTAL = 4096 };
    char tx[TOTAL]; memset(tx, 'A', TOTAL);

    pg_phase(ctx, "sender writes 4096 bytes in ONE call");
    ssize_t w = write(pair.fd_a, tx, TOTAL);
    pg_logf(ctx, "  write() returned %zd", w);

    pg_phase(ctx, "receiver loops on read(); naive code assumes one read suffices");
    pg_expect(ctx, "read_calls", 1);

    int64_t calls = 0, total = 0;
    char    rx[TOTAL];
    /* small idle timeout so the loop terminates if the sender stops */
    struct pollfd pf = { .fd = pair.fd_b, .events = POLLIN };
    while (total < TOTAL) {
        int pr = poll(&pf, 1, 200);
        if (pr <= 0) break;
        ssize_t r = read(pair.fd_b, rx + total, (size_t)(TOTAL - total));
        if (r <= 0) break;
        calls++;
        total += r;
        if (calls <= 10) pg_logf(ctx, "  read() #%lld returned %zd  (cum %lld)",
                                 (long long)calls, r, (long long)total);
    }
    if (calls > 10) pg_logf(ctx, "  ... (suppressed; %lld calls total)", (long long)calls);

    pg_actual(ctx, "read_calls", calls);
    pg_logf(ctx, "needed %lld read() calls to receive %lld bytes",
            (long long)calls, (long long)total);
    pg_logf(ctx, "naive `read(fd, buf, 4096)` would return ~17 and silently truncate your message.");

    chaos_net_close(&pair);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "partial_read",
    .title       = "Network: short reads",
    .one_liner   = "writer sends 4KB; reader gets it 17B at a time",
    .description = "Demonstrates how a single write() arrives as many short reads — the bug that breaks naive socket code.",
    .expected    = "expected: 1 read call; actual: hundreds",
    .lesson      =
        "Why this happens: TCP/UNIX streams have NO message boundaries. A\n"
        "single write() of 4KB can arrive as 100 reads of varying sizes.\n"
        "The kernel coalesces and splits as it sees fit. Code that assumes\n"
        "one read() = one message silently truncates data, or worse, treats\n"
        "the next message's bytes as a continuation.\n"
        "\n"
        "Fixes:\n"
        "  - ALWAYS loop on read() until you have the full expected length.\n"
        "  - Use a length-prefix framing protocol (write 4-byte length\n"
        "    followed by N bytes of payload).\n"
        "  - Or use SOCK_SEQPACKET / message-oriented sockets.\n"
        "  - Higher-level libraries (gRPC, HTTP) handle framing for you.",
    .category    = PG_CAT_NETWORK,
    .run         = pr_run,
};

PG_SCENARIO_REGISTER(scen);
