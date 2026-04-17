#include "playground/scenario.h"
#include "playground/event.h"

#include <stdint.h>
#include <time.h>

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

static int jt_run(pg_runctx_t *ctx, void *state) {
    (void)state;

    enum { N = 50, TARGET_US = 1000 };   /* ask for 1 ms each iteration */
    pg_phase(ctx, "calling nanosleep(1 ms) 50 times; measuring actual delay");
    pg_expect(ctx, "target_us", TARGET_US);

    int64_t total_ns = 0;
    int64_t min_ns   = (int64_t)1 << 62;
    int64_t max_ns   = 0;

    for (int i = 0; i < N; ++i) {
        int64_t t0 = now_ns();
        struct timespec req = { .tv_sec = 0, .tv_nsec = TARGET_US * 1000L };
        nanosleep(&req, NULL);
        int64_t dt = now_ns() - t0;
        total_ns += dt;
        if (dt < min_ns) min_ns = dt;
        if (dt > max_ns) max_ns = dt;
    }
    int64_t mean_us = total_ns / N / 1000;
    int64_t min_us  = min_ns / 1000;
    int64_t max_us  = max_ns / 1000;

    pg_actual(ctx, "mean_us", mean_us);
    pg_gauge (ctx, "min_us",  min_us);
    pg_gauge (ctx, "max_us",  max_us);
    pg_logf  (ctx, "asked for %d us; got mean=%lld us  min=%lld us  max=%lld us",
              TARGET_US, (long long)mean_us, (long long)min_us, (long long)max_us);
    pg_logf  (ctx, "if your code assumes nanosleep(1ms) takes ~1ms, you'll be wrong by %ldx in the worst case.",
              (long)(max_us / TARGET_US));

    return 0;
}

static const pg_scenario_t scen = {
    .id          = "jittery_timer",
    .title       = "Sleep isn't precise",
    .one_liner   = "ask for 1ms; measure what you actually get",
    .description = "Calls nanosleep(1ms) repeatedly and records the actual elapsed time. Min/mean/max are usually wildly different.",
    .expected    = "max delay 5-100x the requested value, depending on system load",
    .lesson      =
        "Why sleeps aren't precise: nanosleep / usleep guarantee that you\n"
        "sleep AT LEAST the requested time, plus scheduler latency. Under\n"
        "load, the actual delay can be many times the request.\n"
        "\n"
        "Fixes:\n"
        "  - Don't sleep for precise timing; compute an absolute deadline\n"
        "    (clock_gettime + offset) and compare against now in a loop.\n"
        "  - For real-time work, use clock_nanosleep with TIMER_ABSTIME, or\n"
        "    busy-wait the last few microseconds.\n"
        "  - Accept jitter as a fact of life; design protocols that tolerate\n"
        "    it (timeouts that are a multiple of the expected interval).",
    .category    = PG_CAT_TIMEIO,
    .run         = jt_run,
};

PG_SCENARIO_REGISTER(scen);
