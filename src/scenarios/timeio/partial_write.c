#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_io.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>

static int pw_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    int fd = chaos_io_open("/tmp/scratch", O_WRONLY);
    if (fd < 0) { pg_fault(ctx, "chaos_io_open failed"); return 1; }
    chaos_io_set_partial(fd, 137);   /* every write returns at most 137 */

    pg_phase(ctx, "naive call: write(fd, buf, 2048) -- expect 2048 back");
    pg_expect(ctx, "naive_returned", 2048);

    char buf[2048];
    memset(buf, 'P', sizeof(buf));
    ssize_t naive = chaos_io_write(fd, buf, sizeof(buf));
    pg_actual(ctx, "naive_returned", (int64_t)naive);
    pg_logf (ctx, "naive write returned %zd of 2048 bytes -- the other 1911 are LOST", naive);

    pg_phase(ctx, "correct call: loop on partial writes");
    size_t  remaining = sizeof(buf);
    char   *p         = buf;
    int     calls     = 0;
    int64_t total     = 0;
    while (remaining > 0) {
        ssize_t w = chaos_io_write(fd, p, remaining);
        if (w <= 0) break;
        calls++;
        total += w;
        p         += w;
        remaining -= (size_t)w;
    }
    pg_expect(ctx, "looped_total", 2048);
    pg_actual(ctx, "looped_total", total);
    pg_count (ctx, "loop_iterations", calls);
    pg_logf  (ctx, "loop wrote %lld bytes in %d calls", (long long)total, calls);

    chaos_io_close(fd);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "partial_write",
    .title       = "Short writes lose data",
    .one_liner   = "write(2048) returns 137; lazy code drops 1911B",
    .description = "Compares naive vs looping write(). The naive call silently loses bytes; the loop succeeds.",
    .expected    = "naive: 137 of 2048; looped: 2048 of 2048",
    .lesson      =
        "Why write() returns less than asked: signal interruption, kernel\n"
        "buffer pressure, slow disks. POSIX guarantees write returns >= 1\n"
        "or -1, NOT that it writes the full request.\n"
        "\n"
        "ALWAYS loop on partial writes:\n"
        "  while (n > 0) {\n"
        "      ssize_t w = write(fd, buf, n);\n"
        "      if (w < 0) { if (errno == EINTR) continue; return -1; }\n"
        "      buf += w; n -= w;\n"
        "  }\n"
        "\n"
        "Same goes for send(2) and read(2)/recv(2).",
    .category    = PG_CAT_TIMEIO,
    .run         = pw_run,
};

PG_SCENARIO_REGISTER(scen);
