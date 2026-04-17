#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/chaos_io.h"

#include <stdint.h>

static int cj_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    pg_phase(ctx, "naive timeout: while (now - start < 100ms)");
    pg_logf (ctx, "we'll start the loop, then have someone jump the clock backward 30s.");
    pg_expect(ctx, "loop_terminated", 1);

    uint64_t start = chaos_clock_now_ns();
    int64_t  iters = 0;
    int      bumped = 0;
    while (chaos_clock_now_ns() - start < 100ULL * 1000000ULL) {
        iters++;
        if (iters == 1000 && !bumped) {
            pg_logf(ctx, "  ...someone bumps the clock backward 30s mid-loop");
            chaos_clock_skew_set(-30LL * 1000000000LL);
            bumped = 1;
        }
        if (iters > 100000000LL) break;   /* safety net */
    }

    int terminated = (iters <= 100000000LL) ? 1 : 0;
    pg_actual(ctx, "loop_terminated", terminated);
    pg_count (ctx, "iterations",     iters);

    if (!terminated) {
        pg_fault(ctx, "loop did NOT terminate -- skew broke the timeout math");
        pg_logf (ctx, "the timeout used `now - start`. After the skew, now < start,");
        pg_logf (ctx, "so the elapsed time is negative and the loop runs forever.");
    } else {
        pg_logf(ctx, "loop terminated after %lld iterations (safety net engaged)", (long long)iters);
    }

    /* reset for cleanliness */
    chaos_clock_skew_set(0);
    return 0;
}

static const pg_scenario_t scen = {
    .id          = "clock_jump",
    .title       = "Clock jumps backward",
    .one_liner   = "skew the clock -30s; watch timeout math break",
    .description = "Mid-loop, the clock jumps backward 30s. The naive `now - start > timeout` check becomes false forever.",
    .expected    = "loop runs past intended deadline (safety net stops it)",
    .lesson      =
        "Why CLOCK_REALTIME is dangerous for elapsed-time math: NTP, leap\n"
        "seconds, and admin changes can jump it backward at any moment.\n"
        "Code that does `now - start > timeout` either becomes false forever\n"
        "(loops infinitely) or fires immediately (false positive).\n"
        "\n"
        "Fix: use CLOCK_MONOTONIC for elapsed-time math. It only goes\n"
        "forward, never jumps. Save CLOCK_REALTIME for displayable wall-\n"
        "clock times you show to humans.\n"
        "\n"
        "On macOS: clock_gettime(CLOCK_MONOTONIC, &ts).\n"
        "On Linux: clock_gettime(CLOCK_MONOTONIC_RAW) is even better\n"
        "(immune to NTP slewing).",
    .category    = PG_CAT_TIMEIO,
    .run         = cj_run,
};

PG_SCENARIO_REGISTER(scen);
