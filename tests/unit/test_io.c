/* Unit tests for chaos_io (quota, partial, fsync delay, clock skew). */

#include "playground/chaos_io.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    /* quota: ENOSPC after limit reached */
    int fd = chaos_io_open("/scratch/quota");
    assert(fd >= 0);
    chaos_io_set_quota(fd, 100);

    char buf[200];
    memset(buf, 'X', sizeof(buf));

    ssize_t w1 = chaos_io_write(fd, buf, 100);
    assert(w1 == 100);

    /* second write hits ENOSPC */
    errno = 0;
    ssize_t w2 = chaos_io_write(fd, buf, 1);
    assert(w2 == -1);
    assert(errno == ENOSPC);

    chaos_io_close(fd);

    /* partial: write returns at most max_chunk */
    int fd2 = chaos_io_open("/scratch/partial");
    assert(fd2 >= 0);
    chaos_io_set_partial(fd2, 17);
    ssize_t w3 = chaos_io_write(fd2, buf, 100);
    assert(w3 == 17);
    chaos_io_close(fd2);

    /* clock skew: setting -30s makes now go backward */
    uint64_t t0 = chaos_clock_now_ns();
    chaos_clock_skew_set(-30LL * 1000000000LL);
    uint64_t t1 = chaos_clock_now_ns();
    /* unsigned subtraction wraps; we just verify t1 != t0 in expected direction */
    assert(t1 < t0);
    chaos_clock_skew_set(0);

    printf("test_io: ok\n");
    return 0;
}
