#pragma once

#include <stdint.h>

typedef struct {
    /* Probability in [0,1] of dropping a chunk in transit (chunks are post-fragmentation). */
    double   loss_prob;
    /* Per-chunk delay drawn uniformly in [delay_min_ns, delay_max_ns]. */
    uint64_t delay_min_ns, delay_max_ns;
    /* If > 0, fragment writes into chunks no larger than this; mimics short reads. */
    int      max_chunk_bytes;
} chaos_net_knobs_t;

typedef struct {
    int   fd_a;        /* user side A — read/write like a normal socket */
    int   fd_b;        /* user side B — read/write like a normal socket */
    void *_internal;   /* opaque proxy state owned by chaos_net */
} chaos_pair_t;

/* Build a bidirectional pair where bytes flowing A→B and B→A are mediated by
 * proxy threads that apply loss/delay/fragmentation. The user code uses fd_a
 * and fd_b like ordinary stream sockets. Returns 0 on success, -1 on failure. */
int  chaos_net_pair (chaos_pair_t *out, const chaos_net_knobs_t *k, uint64_t seed);

/* Stop the proxy threads, close all fds, free internal state. */
void chaos_net_close(chaos_pair_t *p);
