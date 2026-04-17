/* Unit tests for chaos_net (clean delivery with zero knobs, loss with knobs). */

#include "playground/chaos_net.h"

#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int read_until(int fd, void *buf, size_t want) {
    size_t got = 0;
    char  *p   = (char *)buf;
    while (got < want) {
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int pr = poll(&pf, 1, 1000);
        if (pr <= 0) break;
        ssize_t r = read(fd, p + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

int main(void) {
    /* zero knobs: bytes arrive intact */
    chaos_pair_t      p;
    chaos_net_knobs_t k = { 0 };
    assert(chaos_net_pair(&p, &k, 42) == 0);

    char tx[1024];
    for (int i = 0; i < 1024; ++i) tx[i] = (char)(i & 0xFF);

    ssize_t w = write(p.fd_a, tx, sizeof(tx));
    assert(w == (ssize_t)sizeof(tx));

    char rx[1024];
    int  got = read_until(p.fd_b, rx, sizeof(rx));
    assert(got == (int)sizeof(rx));
    assert(memcmp(tx, rx, sizeof(tx)) == 0);

    chaos_net_close(&p);

    /* 100% loss: nothing arrives */
    chaos_pair_t      p2;
    chaos_net_knobs_t k2 = { .loss_prob = 1.0, .max_chunk_bytes = 16 };
    assert(chaos_net_pair(&p2, &k2, 42) == 0);
    char small[64];
    memset(small, 'L', sizeof(small));
    write(p2.fd_a, small, sizeof(small));

    char rx2[64];
    struct pollfd pf = { .fd = p2.fd_b, .events = POLLIN };
    int pr = poll(&pf, 1, 200);
    /* Either timeout (POLLIN never set) or 0-byte read indicating EOF after all
     * chunks were dropped — both signal "nothing arrived". */
    if (pr > 0) {
        ssize_t r = read(p2.fd_b, rx2, sizeof(rx2));
        assert(r <= 0);
    }
    chaos_net_close(&p2);

    printf("test_net: ok\n");
    return 0;
}
