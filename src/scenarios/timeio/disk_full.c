#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>

static int df_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    int fd = chaos_io_open("/tmp/scratch");
    if (fd < 0) { pg_sut_fault(ctx, "chaos_io_open failed"); return 1; }
    chaos_io_set_quota(fd, 1024);   /* 1 KiB quota */

    pg_phase(ctx, "writing 64 chunks of 64 bytes; quota is 1024 bytes");
    pg_expect(ctx, "bytes_written_ok", 1024);

    char    buf[64];
    memset(buf, 'D', sizeof(buf));
    int64_t total          = 0;
    int     enospc_count   = 0;
    int     first_enospc_i = -1;

    for (int i = 0; i < 64; ++i) {
        ssize_t w = chaos_io_write(fd, buf, sizeof(buf));
        if (w < 0) {
            if (errno == ENOSPC) {
                if (first_enospc_i < 0) first_enospc_i = i;
                enospc_count++;
            } else {
                pg_logf(ctx, "iter %d: write() failed errno=%d", i, errno);
            }
        } else {
            total += w;
        }
    }

    pg_actual(ctx, "bytes_written_ok", total);
    pg_count (ctx, "enospc_errors",    enospc_count);
    pg_logf  (ctx, "first ENOSPC at iteration %d; %d total ENOSPC errors",
              first_enospc_i, enospc_count);
    pg_logf  (ctx, "code that ignores the return value of write() silently loses data here.");

    chaos_io_close(fd);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "disk_full",
    .title       = "Disk full: ENOSPC",
    .one_liner   = "write past quota; observe ENOSPC handling",
    .description = "A wrapped fd reports ENOSPC after 1KiB has been written. Demonstrates silent data loss when write() returns are ignored.",
    .expected    = "first 16 writes succeed (1024B), rest return -1/ENOSPC",
    .lesson      =
        "Why this loses data: write() returning -1 with ENOSPC means YOUR\n"
        "DATA DIDN'T MAKE IT. Code that ignores write()'s return value\n"
        "silently drops bytes and reports success. Logs, transactions,\n"
        "user data -- all gone, no warning.\n"
        "\n"
        "Fixes:\n"
        "  - Always check write()'s return value. -1 means failure.\n"
        "  - Loop on partial writes (write may return < n even on success).\n"
        "  - On ENOSPC, fail loudly. Don't continue as if nothing happened.\n"
        "  - For critical data: monitor free space proactively, alert before\n"
        "    you hit zero.",
    .category    = PG_CAT_TIMEIO,
    .run         = df_run,
};

PG_SCENARIO_REGISTER(scen);
