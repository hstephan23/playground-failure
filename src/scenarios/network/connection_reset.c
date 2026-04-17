#include "playground/scenario.h"
#include "playground/event.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int          fd;
    int          delay_ms;
    pg_runctx_t *ctx;
} peer_arg_t;

static void *peer_fn(void *arg) {
    peer_arg_t *pa = (peer_arg_t *)arg;
    char rbuf[4096];
    /* read briefly, then slam the door */
    ssize_t r = read(pa->fd, rbuf, sizeof(rbuf));
    pg_logf(pa->ctx, "peer received %zd bytes; closing connection in %d ms",
            r, pa->delay_ms);
    struct timespec ts = { .tv_nsec = (long)pa->delay_ms * 1000000L };
    nanosleep(&ts, NULL);
    close(pa->fd);
    pg_logf(pa->ctx, "peer closed");
    return NULL;
}

static int cr_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
        pg_sut_fault(ctx, "socketpair failed");
        return 1;
    }
    int my_fd   = sp[0];
    int peer_fd = sp[1];

    /* Otherwise SIGPIPE kills the process when we write to a closed peer. */
    signal(SIGPIPE, SIG_IGN);

    pg_phase(ctx, "spawning peer; it will read once then close after 30 ms");
    peer_arg_t pa = { .fd = peer_fd, .delay_ms = 30, .ctx = ctx };
    pthread_t  pt;
    pthread_create(&pt, NULL, peer_fn, &pa);

    pg_phase(ctx, "main thread writes 4KB chunks until something breaks");
    char buf[4096];
    memset(buf, 'C', sizeof(buf));
    int64_t total          = 0;
    int     errors         = 0;
    int     first_err_iter = -1;
    int     first_errno    = 0;

    for (int i = 0; i < 1000; ++i) {
        ssize_t w = write(my_fd, buf, sizeof(buf));
        if (w < 0) {
            if (first_err_iter < 0) {
                first_err_iter = i;
                first_errno    = errno;
                pg_logf(ctx, "first write error at iter %d: errno=%d (%s)",
                        i, errno, strerror(errno));
            }
            errors++;
            if (errors >= 3) break;
        } else {
            total += w;
        }
        sched_yield();
    }

    pthread_join(pt, NULL);
    close(my_fd);

    pg_actual(ctx, "bytes_before_reset", total);
    pg_actual(ctx, "first_error_iter",   (int64_t)first_err_iter);
    pg_count (ctx, "write_errors",       errors);

    pg_logf(ctx, "wrote %lld bytes before peer close; %d write errors",
            (long long)total, errors);

    if (first_errno == EPIPE) {
        pg_logf(ctx, "EPIPE = peer's read side is gone. Without SIGPIPE-ignore,");
        pg_logf(ctx, "this would have killed the process before write() returned.");
    }
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "connection_reset",
    .title       = "Peer disappears mid-stream",
    .one_liner   = "peer closes after 30ms; we keep writing",
    .description = "Demonstrates write returning EPIPE when the peer closes the connection mid-stream.",
    .expected    = "first writes succeed; later writes return -1 with EPIPE",
    .lesson      =
        "Why this happens: TCP/UNIX peers vanish at any time — process crash,\n"
        "network partition, idle timeout. write() returns EPIPE; read() returns\n"
        "0 (EOF) potentially mid-message.\n"
        "\n"
        "Fixes:\n"
        "  - signal(SIGPIPE, SIG_IGN) so write returns -1 instead of killing\n"
        "    the process.\n"
        "  - Always check return values; treat EOF mid-message as an error,\n"
        "    not a successful read.\n"
        "  - Use heartbeats / keepalives to detect dead peers proactively.",
    .category    = PG_CAT_NETWORK,
    .run         = cr_run,
};

PG_SCENARIO_REGISTER(scen);
