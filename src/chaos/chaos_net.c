#include "playground/chaos_net.h"
#include "core/rand.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int                proxy_a;     /* paired with user fd_a; we read user→proxy bytes here */
    int                proxy_b;     /* paired with user fd_b; we write proxy→user bytes here */
    chaos_net_knobs_t  knobs;
    pthread_t          t_a_to_b;
    pthread_t          t_b_to_a;
    pg_xosh_t          rng_a, rng_b;
    int                running;
} pair_internal_t;

typedef struct {
    int        src_fd;
    int        dst_fd;
    pair_internal_t *I;
    pg_xosh_t *rng;
} forward_arg_t;

static double rand_unit(pg_xosh_t *r) {
    /* Map the top 53 bits of the RNG to [0, 1). */
    uint64_t v = pg_xosh_next(r) >> 11;
    return (double)v / (double)(1ULL << 53);
}

static void apply_delay(pg_xosh_t *rng, uint64_t min_ns, uint64_t max_ns) {
    uint64_t span = max_ns > min_ns ? max_ns - min_ns : 0;
    uint64_t d    = min_ns + (span ? pg_xosh_next(rng) % span : 0);
    if (d == 0) return;
    struct timespec ts = {
        .tv_sec  = (time_t)(d / 1000000000ULL),
        .tv_nsec = (long)  (d % 1000000000ULL),
    };
    nanosleep(&ts, NULL);
}

static void *forward_fn(void *arg) {
    forward_arg_t *fa  = (forward_arg_t *)arg;
    pair_internal_t *I = fa->I;
    char buf[4096];

    while (I->running) {
        ssize_t n = read(fa->src_fd, buf, sizeof(buf));
        if (n <= 0) break;

        size_t chunk_max = I->knobs.max_chunk_bytes > 0
            ? (size_t)I->knobs.max_chunk_bytes
            : (size_t)n;

        size_t off = 0;
        while (off < (size_t)n) {
            size_t this = (size_t)n - off;
            if (this > chunk_max) this = chunk_max;

            /* loss */
            if (I->knobs.loss_prob > 0.0) {
                if (rand_unit(fa->rng) < I->knobs.loss_prob) {
                    off += this;
                    continue;
                }
            }
            /* delay */
            if (I->knobs.delay_max_ns > 0) {
                apply_delay(fa->rng, I->knobs.delay_min_ns, I->knobs.delay_max_ns);
            }

            ssize_t w = write(fa->dst_fd, buf + off, this);
            if (w <= 0) goto out;
            off += (size_t)w;
        }
    }
out:
    /* Closing the write side signals EOF to the user reader. */
    shutdown(fa->dst_fd, SHUT_WR);
    free(fa);
    return NULL;
}

int chaos_net_pair(chaos_pair_t *out, const chaos_net_knobs_t *k, uint64_t seed) {
    if (!out) { errno = EINVAL; return -1; }
    int sp1[2], sp2[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp1) < 0) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp2) < 0) {
        close(sp1[0]); close(sp1[1]); return -1;
    }

    pair_internal_t *I = (pair_internal_t *)calloc(1, sizeof(*I));
    if (!I) {
        close(sp1[0]); close(sp1[1]); close(sp2[0]); close(sp2[1]);
        errno = ENOMEM; return -1;
    }
    I->knobs   = k ? *k : (chaos_net_knobs_t){0};
    I->proxy_a = sp1[1];
    I->proxy_b = sp2[0];
    I->running = 1;
    pg_xosh_seed(&I->rng_a, seed ^ 0xA1A1A1A1A1A1A1A1ULL);
    pg_xosh_seed(&I->rng_b, seed ^ 0xB2B2B2B2B2B2B2B2ULL);

    out->fd_a       = sp1[0];
    out->fd_b       = sp2[1];
    out->_internal  = I;

    /* A → B forwarder: read user_a (via proxy_a), write to proxy_b (-> user_b) */
    forward_arg_t *fa = (forward_arg_t *)malloc(sizeof(*fa));
    fa->src_fd = I->proxy_a;
    fa->dst_fd = I->proxy_b;
    fa->I      = I;
    fa->rng    = &I->rng_a;
    if (pthread_create(&I->t_a_to_b, NULL, forward_fn, fa) != 0) {
        free(fa); free(I);
        close(sp1[0]); close(sp1[1]); close(sp2[0]); close(sp2[1]);
        return -1;
    }

    /* B → A forwarder */
    forward_arg_t *fb = (forward_arg_t *)malloc(sizeof(*fb));
    fb->src_fd = I->proxy_b;
    fb->dst_fd = I->proxy_a;
    fb->I      = I;
    fb->rng    = &I->rng_b;
    if (pthread_create(&I->t_b_to_a, NULL, forward_fn, fb) != 0) {
        free(fb);
        I->running = 0;
        shutdown(I->proxy_a, SHUT_RDWR);
        pthread_join(I->t_a_to_b, NULL);
        free(I);
        close(sp1[0]); close(sp1[1]); close(sp2[0]); close(sp2[1]);
        return -1;
    }

    return 0;
}

void chaos_net_close(chaos_pair_t *p) {
    if (!p || !p->_internal) return;
    pair_internal_t *I = (pair_internal_t *)p->_internal;
    I->running = 0;
    /* Closing the user fds drops EOF on the proxy reads, which terminates them. */
    if (p->fd_a >= 0) { close(p->fd_a); p->fd_a = -1; }
    if (p->fd_b >= 0) { close(p->fd_b); p->fd_b = -1; }
    pthread_join(I->t_a_to_b, NULL);
    pthread_join(I->t_b_to_a, NULL);
    if (I->proxy_a >= 0) close(I->proxy_a);
    if (I->proxy_b >= 0) close(I->proxy_b);
    free(I);
    p->_internal = NULL;
}
