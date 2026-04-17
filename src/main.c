#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef PG_HAS_TUI
int tui_run(void);
#endif

static void print_usage(FILE *f, const char *argv0) {
    fprintf(f,
        "usage: %s [--scenario ID] [--seed N] [--json] [--list]\n"
        "  default (no flags) : launches interactive TUI (if built in)\n"
        "  --scenario ID      : run one scenario headless and stream events\n"
        "  --seed N           : seed the deterministic RNG (default: time(NULL))\n"
        "  --json             : emit one JSON object per event (otherwise plain text)\n"
        "  --list             : list registered scenarios and exit\n",
        argv0);
}

static void list_scenarios(void) {
    size_t n;
    const pg_scenario_t **all = pg_registry_list(&n);
    if (n == 0) {
        printf("(no scenarios registered)\n");
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        printf("  %-14s  %-32s  [%s]\n",
            all[i]->id, all[i]->title, pg_category_name(all[i]->category));
    }
}

/* Escape a string for JSON. Quick-and-dirty: escapes \ " and control chars. */
static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else          fputc((int)c, f);
        }
    }
    fputc('"', f);
}

static int run_headless(const char *id, uint64_t seed, int json) {
    const pg_scenario_t *s = pg_registry_find(id);
    if (!s) {
        fprintf(stderr, "unknown scenario: %s\n", id);
        return 2;
    }

    pg_run_handle_t h;
    if (pg_run_start(s, seed, &h) < 0) {
        fprintf(stderr, "failed to start scenario\n");
        return 1;
    }

    pg_event_t ev;
    int        r;
    int        saw_done = 0;
    while ((r = pg_run_poll(&h, &ev, -1)) > 0) {
        if (json) {
            fprintf(stdout, "{\"kind\":");
            json_escape(stdout, pg_event_kind_name((pg_event_kind_t)ev.kind));
            fprintf(stdout, ",\"ts_ns\":%llu,\"thread\":%u,\"key\":",
                (unsigned long long)ev.ts_ns, ev.thread_tag);
            json_escape(stdout, ev.key);
            fprintf(stdout, ",\"num\":%lld,\"text\":", (long long)ev.num);
            json_escape(stdout, ev.text);
            fprintf(stdout, "}\n");
        } else {
            double tsec = (double)ev.ts_ns / 1e9;
            printf("%9.3f  %-8s  t=%-2u  %-16s  num=%-10lld  %s\n",
                tsec,
                pg_event_kind_name((pg_event_kind_t)ev.kind),
                ev.thread_tag, ev.key, (long long)ev.num, ev.text);
        }
        fflush(stdout);
        if (ev.kind == PG_EV_DONE) { saw_done = 1; break; }
    }

    int status = 0;
    pg_run_reap(&h, &status);

    if (!saw_done) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "child died from signal %d\n", WTERMSIG(status));
        } else if (WIFEXITED(status)) {
            fprintf(stderr, "child exited %d before PG_EV_DONE\n", WEXITSTATUS(status));
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *scenario_id = NULL;
    uint64_t    seed        = (uint64_t)time(NULL);
    int         json        = 0;
    int         list        = 0;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--scenario") && i + 1 < argc) {
            scenario_id = argv[++i];
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            seed = strtoull(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--json")) {
            json = 1;
        } else if (!strcmp(argv[i], "--list")) {
            list = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(stdout, argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            print_usage(stderr, argv[0]);
            return 2;
        }
    }

    if (list) { list_scenarios(); return 0; }

    if (scenario_id) {
        return run_headless(scenario_id, seed, json);
    }

#ifdef PG_HAS_TUI
    return tui_run();
#else
    fprintf(stderr,
        "this binary is headless-only. pass --scenario ID, or use ./playground.\n");
    print_usage(stderr, argv[0]);
    return 2;
#endif
}
