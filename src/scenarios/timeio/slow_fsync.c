#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_io.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>

static int sf_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    int fd = chaos_io_open("/tmp/scratch", O_WRONLY);
    if (fd < 0) { pg_fault(ctx, "chaos_io_open failed"); return 1; }
    chaos_io_set_fsync_delay(fd, 1500ULL * 1000000ULL);   /* 1.5 s */

    pg_phase(ctx, "writing 1 KiB");
    char buf[1024]; memset(buf, 'S', sizeof(buf));
    chaos_io_write(fd, buf, sizeof(buf));

    pg_phase(ctx, "calling fsync (configured to take 1500 ms)");
    pg_expect(ctx, "fsync_ms", 1500);

    uint64_t t0 = chaos_clock_now_ns();
    chaos_io_fsync(fd);
    uint64_t dt = chaos_clock_now_ns() - t0;
    int64_t  ms = (int64_t)(dt / 1000000ULL);

    pg_actual(ctx, "fsync_ms", ms);
    pg_logf (ctx, "fsync returned after %lld ms", (long long)ms);
    pg_logf (ctx, "if this were on the UI thread, the app would freeze for %lld ms.", (long long)ms);
    pg_logf (ctx, "real-world fsync latency on a busy disk routinely hits seconds.");

    chaos_io_close(fd);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "slow_fsync",
    .title       = "Slow fsync: UI freeze",
    .one_liner   = "fsync takes 1.5s; observe what blocks",
    .description = "A wrapped fd makes fsync(2) block for a configurable duration. Demonstrates UI freeze patterns.",
    .expected    = "fsync_ms ~= 1500; observable wall-clock delay",
    .lesson      =
        "Why this matters: fsync waits for actual disk persistence. On a\n"
        "busy disk this is seconds. On a slow disk under load, it can be\n"
        "tens of seconds. Calling fsync on the UI thread freezes the app.\n"
        "Calling it inside a hot loop tanks throughput.\n"
        "\n"
        "Fixes:\n"
        "  - Move fsync to a background thread; don't block the UI on it.\n"
        "  - Batch writes between fsyncs (group commit pattern).\n"
        "  - Use O_DSYNC if you only need data persistence, not metadata.\n"
        "  - Use fdatasync() instead of fsync() to skip metadata sync.\n"
        "  - For some workloads (logs, caches), fsync is overkill -- the\n"
        "    cost of losing the last few seconds of data is acceptable.",
    .category    = PG_CAT_TIMEIO,
    .run         = sf_run,
};

PG_SCENARIO_REGISTER(scen);
