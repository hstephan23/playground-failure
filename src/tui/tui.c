#include "playground/scenario.h"
#include "playground/event.h"
#include "playground/runner.h"

#include <ncurses.h>

#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---------------- color pairs ---------------- */

enum {
    CP_NORMAL = 1,
    CP_CHAOS  = 2,
    CP_FAULT  = 3,
    CP_HEADER = 4,
    CP_DIM    = 5,
    CP_SELECT = 6,
    CP_BAR    = 7,
    CP_MATCH  = 8,
    CP_MISS   = 9,
};

static int g_has_color = 0;

static void init_colors(void) {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_NORMAL, COLOR_GREEN,   -1);
    init_pair(CP_CHAOS,  COLOR_YELLOW,  -1);
    init_pair(CP_FAULT,  COLOR_RED,     -1);
    init_pair(CP_HEADER, COLOR_CYAN,    -1);
    init_pair(CP_DIM,    COLOR_WHITE,   -1);
    init_pair(CP_SELECT, COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_BAR,    COLOR_GREEN,   -1);
    init_pair(CP_MATCH,  COLOR_GREEN,   -1);
    init_pair(CP_MISS,   COLOR_RED,     -1);
    g_has_color = 1;
}

static void pg_attr_on(WINDOW *w, int cp, int attr) {
    if (g_has_color && cp) wattron(w, COLOR_PAIR(cp));
    if (attr) wattron(w, attr);
}
static void pg_attr_off(WINDOW *w, int cp, int attr) {
    if (g_has_color && cp) wattroff(w, COLOR_PAIR(cp));
    if (attr) wattroff(w, attr);
}

/* ---------------- ring-buffered, colored event log ---------------- */

#define LOG_LINES     512
#define LOG_LINE_LEN  256

typedef struct {
    char text[LOG_LINE_LEN];
    int  cp;
    int  attr;
} log_entry_t;

static log_entry_t g_log[LOG_LINES];
static unsigned    g_log_count = 0;

static void log_clear(void) { g_log_count = 0; }

static void log_pushf(int cp, int attr, const char *fmt, ...) {
    log_entry_t *e = &g_log[g_log_count % LOG_LINES];
    e->cp = cp;
    e->attr = attr;
    va_list ap; va_start(ap, fmt);
    vsnprintf(e->text, LOG_LINE_LEN, fmt, ap);
    va_end(ap);
    g_log_count++;
}

/* ---------------- stats table ---------------- */

#define STATS_MAX 64

typedef struct {
    char    key[PG_EVENT_KEY_LEN];
    int64_t value;
    int64_t gauge_min, gauge_max;
    int     touched_gauge;
    int     kind;
} stat_t;

static stat_t g_stats[STATS_MAX];
static int    g_stats_n = 0;

static void stats_clear(void) { g_stats_n = 0; }

static void stat_upsert(const char *key, int64_t v, int kind, int delta) {
    for (int i = 0; i < g_stats_n; ++i) {
        if (g_stats[i].kind == kind && strcmp(g_stats[i].key, key) == 0) {
            g_stats[i].value = delta ? g_stats[i].value + v : v;
            if (kind == PG_EV_GAUGE) {
                if (!g_stats[i].touched_gauge || v < g_stats[i].gauge_min) g_stats[i].gauge_min = v;
                if (!g_stats[i].touched_gauge || v > g_stats[i].gauge_max) g_stats[i].gauge_max = v;
                g_stats[i].touched_gauge = 1;
            }
            return;
        }
    }
    if (g_stats_n < STATS_MAX) {
        snprintf(g_stats[g_stats_n].key, sizeof(g_stats[g_stats_n].key), "%s", key);
        g_stats[g_stats_n].value = v;
        g_stats[g_stats_n].kind  = kind;
        if (kind == PG_EV_GAUGE) {
            g_stats[g_stats_n].gauge_min = v;
            g_stats[g_stats_n].gauge_max = v;
            g_stats[g_stats_n].touched_gauge = 1;
        }
        g_stats_n++;
    }
}

/* ---------------- windows ---------------- */

static WINDOW *win_menu, *win_log, *win_stats, *win_status;

static const pg_scenario_t **g_scens   = NULL;
static size_t                 g_scen_n = 0;
static int                    g_sel    = 0;

static void touch_all(void) {
    if (win_menu)   touchwin(win_menu);
    if (win_log)    touchwin(win_log);
    if (win_stats)  touchwin(win_stats);
    if (win_status) touchwin(win_status);
}

/* ---------------- menu pane ---------------- */

static int category_color(pg_category_t c) {
    switch (c) {
    case PG_CAT_CONCURRENCY: return CP_CHAOS;
    case PG_CAT_MEMORY:      return CP_FAULT;
    case PG_CAT_NETWORK:     return CP_HEADER;
    case PG_CAT_TIMEIO:      return CP_NORMAL;
    default:                 return CP_NORMAL;
    }
}

static void draw_menu(void) {
    werase(win_menu);
    box(win_menu, 0, 0);

    pg_attr_on(win_menu, CP_HEADER, A_BOLD);
    mvwprintw(win_menu, 0, 2, " Failure Playground ");
    pg_attr_off(win_menu, CP_HEADER, A_BOLD);

    int rows, cols; getmaxyx(win_menu, rows, cols); (void)rows;
    int w = cols - 4;

    int top = 2;
    for (size_t i = 0; i < g_scen_n; ++i) {
        int row = top + (int)i;
        int is_sel = (int)i == g_sel;
        int cp     = is_sel ? CP_SELECT : category_color(g_scens[i]->category);
        int attr   = is_sel ? A_BOLD    : 0;
        pg_attr_on(win_menu, cp, attr);
        mvwprintw(win_menu, row, 2, " %-*.*s ", w - 1, w - 1, g_scens[i]->title);
        pg_attr_off(win_menu, cp, attr);
    }

    if (g_scen_n > 0) {
        const pg_scenario_t *s = g_scens[g_sel];
        int y = top + (int)g_scen_n + 1;
        pg_attr_on(win_menu, CP_DIM, 0);
        mvwprintw(win_menu, y,     2, "%-*.*s", w, w, s->one_liner);
        mvwprintw(win_menu, y + 1, 2, "expect: %-*.*s", w - 8, w - 8, s->expected);
        pg_attr_off(win_menu, CP_DIM, 0);
    }
    wnoutrefresh(win_menu);
}

/* ---------------- event log pane ---------------- */

static void draw_log(void) {
    werase(win_log);
    box(win_log, 0, 0);
    pg_attr_on(win_log, CP_HEADER, A_BOLD);
    mvwprintw(win_log, 0, 2, " Event Log ");
    pg_attr_off(win_log, CP_HEADER, A_BOLD);

    int rows, cols; getmaxyx(win_log, rows, cols);
    int viewable = rows - 2;
    if (viewable < 1) { wnoutrefresh(win_log); return; }

    unsigned start = g_log_count > (unsigned)viewable
        ? g_log_count - (unsigned)viewable : 0;

    for (int i = 0; i < viewable && start + (unsigned)i < g_log_count; ++i) {
        unsigned idx = (start + (unsigned)i) % LOG_LINES;
        log_entry_t *e = &g_log[idx];
        int w = cols - 2;
        pg_attr_on(win_log, e->cp, e->attr);
        mvwprintw(win_log, 1 + i, 1, "%-*.*s", w, w, e->text);
        pg_attr_off(win_log, e->cp, e->attr);
    }
    wnoutrefresh(win_log);
}

/* ---------------- stats pane ---------------- */

static void draw_bar(WINDOW *w, int y, int x, int width, double frac) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int filled = (int)(frac * (double)width);
    if (filled > width) filled = width;
    pg_attr_on(w, CP_BAR, 0);
    for (int i = 0; i < filled; ++i) mvwaddch(w, y, x + i, '#');
    pg_attr_off(w, CP_BAR, 0);
    pg_attr_on(w, CP_DIM, 0);
    for (int i = filled; i < width; ++i) mvwaddch(w, y, x + i, '.');
    pg_attr_off(w, CP_DIM, 0);
}

static void draw_stats(void) {
    werase(win_stats);
    box(win_stats, 0, 0);
    pg_attr_on(win_stats, CP_HEADER, A_BOLD);
    mvwprintw(win_stats, 0, 2, " Stats ");
    pg_attr_off(win_stats, CP_HEADER, A_BOLD);

    int rows, cols; getmaxyx(win_stats, rows, cols);
    int row = 1;

    int has_counters = 0, has_gauges = 0, has_diff = 0;
    for (int i = 0; i < g_stats_n; ++i) {
        if (g_stats[i].kind == PG_EV_COUNTER) has_counters = 1;
        if (g_stats[i].kind == PG_EV_GAUGE)   has_gauges   = 1;
        if (g_stats[i].kind == PG_EV_EXPECT || g_stats[i].kind == PG_EV_ACTUAL) has_diff = 1;
    }

    if (has_counters && row < rows - 1) {
        pg_attr_on(win_stats, CP_HEADER, A_BOLD);
        mvwprintw(win_stats, row++, 1, "COUNTERS");
        pg_attr_off(win_stats, CP_HEADER, A_BOLD);
        for (int i = 0; i < g_stats_n && row < rows - 1; ++i) {
            if (g_stats[i].kind != PG_EV_COUNTER) continue;
            mvwprintw(win_stats, row++, 2, "%-18s %lld",
                g_stats[i].key, (long long)g_stats[i].value);
        }
        if (row < rows - 1) row++;
    }

    if (has_gauges && row < rows - 1) {
        pg_attr_on(win_stats, CP_HEADER, A_BOLD);
        mvwprintw(win_stats, row++, 1, "GAUGES");
        pg_attr_off(win_stats, CP_HEADER, A_BOLD);
        int label_w   = 14;
        int value_w   = 12;
        int bar_width = cols - label_w - value_w - 4;
        if (bar_width < 6) bar_width = 6;
        for (int i = 0; i < g_stats_n && row < rows - 1; ++i) {
            if (g_stats[i].kind != PG_EV_GAUGE) continue;
            int64_t v   = g_stats[i].value;
            int64_t lo  = g_stats[i].gauge_min;
            int64_t hi  = g_stats[i].gauge_max;
            int64_t span = hi - lo;
            double  frac = span > 0 ? (double)(v - lo) / (double)span : 1.0;
            mvwprintw(win_stats, row, 1, " %-*.*s ", label_w - 2, label_w - 2, g_stats[i].key);
            draw_bar(win_stats, row, 1 + label_w, bar_width, frac);
            mvwprintw(win_stats, row, 1 + label_w + bar_width + 1, "%lld", (long long)v);
            row++;
        }
        if (row < rows - 1) row++;
    }

    if (has_diff && row < rows - 1) {
        pg_attr_on(win_stats, CP_HEADER, A_BOLD);
        mvwprintw(win_stats, row++, 1, "EXPECTED vs ACTUAL");
        pg_attr_off(win_stats, CP_HEADER, A_BOLD);
        for (int i = 0; i < g_stats_n && row < rows - 1; ++i) {
            if (g_stats[i].kind != PG_EV_EXPECT) continue;
            int64_t exp = g_stats[i].value;
            int64_t act = 0;
            int     have_act = 0;
            for (int j = 0; j < g_stats_n; ++j) {
                if (g_stats[j].kind == PG_EV_ACTUAL && strcmp(g_stats[j].key, g_stats[i].key) == 0) {
                    act = g_stats[j].value;
                    have_act = 1;
                    break;
                }
            }
            int cp_out = !have_act ? CP_DIM : (exp == act ? CP_MATCH : CP_MISS);
            int attr   = have_act ? A_BOLD : 0;
            pg_attr_on(win_stats, cp_out, attr);
            if (have_act) {
                mvwprintw(win_stats, row++, 2, "%-14s  %lld vs %lld  (%+lld)",
                    g_stats[i].key, (long long)exp, (long long)act, (long long)(act - exp));
            } else {
                mvwprintw(win_stats, row++, 2, "%-14s  %lld vs (pending)",
                    g_stats[i].key, (long long)exp);
            }
            pg_attr_off(win_stats, cp_out, attr);
        }
    }

    wnoutrefresh(win_stats);
}

/* ---------------- status line ---------------- */

static void draw_status(const char *text) {
    werase(win_status);
    int rows, cols; getmaxyx(win_status, rows, cols); (void)rows;
    pg_attr_on(win_status, CP_DIM, 0);
    mvwprintw(win_status, 0, 1, "%-*.*s", cols - 2, cols - 2, text);
    pg_attr_off(win_status, CP_DIM, 0);
    wnoutrefresh(win_status);
}

/* ---------------- modal overlay ---------------- */

static void show_overlay(const char *title, const char *body) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int oh = rows > 28 ? 24 : (rows > 8 ? rows - 4 : rows);
    int ow = cols > 90 ? 80 : (cols > 12 ? cols - 8 : cols);
    int oy = (rows - oh) / 2;
    int ox = (cols - ow) / 2;

    WINDOW *ov = newwin(oh, ow, oy, ox);
    box(ov, 0, 0);
    pg_attr_on(ov, CP_HEADER, A_BOLD);
    mvwprintw(ov, 0, 2, " %s ", title ? title : "Lesson");
    pg_attr_off(ov, CP_HEADER, A_BOLD);

    int       y    = 2;
    int       wrap = ow - 4;
    const char *p  = body && *body ? body : "(no lesson written for this scenario)";
    while (*p && y < oh - 2) {
        const char *nl  = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        while (len > (size_t)wrap && y < oh - 2) {
            mvwprintw(ov, y++, 2, "%.*s", wrap, p);
            p   += wrap;
            len -= (size_t)wrap;
        }
        if (y < oh - 2) {
            mvwprintw(ov, y++, 2, "%.*s", (int)len, p);
        }
        if (nl) p = nl + 1; else break;
    }

    pg_attr_on(ov, CP_DIM, 0);
    mvwprintw(ov, oh - 2, 2, "(press any key)");
    pg_attr_off(ov, CP_DIM, 0);
    wrefresh(ov);

    nodelay(stdscr, FALSE);
    (void)getch();
    nodelay(stdscr, TRUE);
    delwin(ov);
    touch_all();
}

/* ---------------- event styling ---------------- */

typedef struct { int cp; int attr; } style_t;

static style_t style_for(pg_event_kind_t k) {
    switch (k) {
    case PG_EV_LOG:         return (style_t){ CP_NORMAL, 0 };
    case PG_EV_PHASE:       return (style_t){ CP_HEADER, A_BOLD };
    case PG_EV_COUNTER:     return (style_t){ CP_DIM,    0 };
    case PG_EV_GAUGE:       return (style_t){ CP_DIM,    0 };
    case PG_EV_EXPECT:      return (style_t){ CP_CHAOS,  0 };
    case PG_EV_ACTUAL:      return (style_t){ CP_CHAOS,  A_BOLD };
    case PG_EV_SUT_FAULT:   return (style_t){ CP_FAULT,  A_BOLD };
    case PG_EV_CHILD_CRASH: return (style_t){ CP_FAULT,  A_BOLD };
    case PG_EV_WATCHDOG:    return (style_t){ CP_FAULT,  A_BOLD };
    case PG_EV_DONE:        return (style_t){ CP_NORMAL, A_BOLD };
    default:                return (style_t){ CP_NORMAL, 0 };
    }
}

static void push_event(const pg_event_t *ev) {
    style_t s = style_for((pg_event_kind_t)ev->kind);
    double  t = (double)ev->ts_ns / 1e9;
    switch (ev->kind) {
    case PG_EV_LOG:
        log_pushf(s.cp, s.attr, "%6.3f log[t%u] %s", t, ev->thread_tag, ev->text);
        break;
    case PG_EV_PHASE:
        log_pushf(s.cp, s.attr, "%6.3f -- %s --", t, ev->text);
        break;
    case PG_EV_COUNTER:
        log_pushf(s.cp, s.attr, "%6.3f counter %s += %lld", t, ev->key, (long long)ev->num);
        break;
    case PG_EV_GAUGE:
        log_pushf(s.cp, s.attr, "%6.3f gauge   %s = %lld", t, ev->key, (long long)ev->num);
        break;
    case PG_EV_EXPECT:
        log_pushf(s.cp, s.attr, "%6.3f expect  %s = %lld", t, ev->key, (long long)ev->num);
        break;
    case PG_EV_ACTUAL:
        log_pushf(s.cp, s.attr, "%6.3f actual  %s = %lld", t, ev->key, (long long)ev->num);
        break;
    case PG_EV_SUT_FAULT:
        log_pushf(s.cp, s.attr, "%6.3f SUT FAULT  %s", t, ev->text);
        break;
    case PG_EV_CHILD_CRASH:
        log_pushf(s.cp, s.attr, "%6.3f CRASH      %s", t, ev->text);
        break;
    case PG_EV_WATCHDOG:
        log_pushf(s.cp, s.attr, "%6.3f WATCHDOG   %s", t, ev->text);
        break;
    case PG_EV_DONE:
        log_pushf(s.cp, s.attr, "%6.3f done.", t);
        break;
    default:
        log_pushf(s.cp, s.attr, "%6.3f ?", t);
    }
}

/* ---------------- run a scenario ---------------- */

static int g_last_exit = 0;
static uint64_t g_last_seed = 0;

static const char *running_status =
    "running...   [q] abort";

static void redraw_all(const char *status) {
    draw_menu();
    draw_log();
    draw_stats();
    draw_status(status);
    doupdate();
}

static int run_scenario(const pg_scenario_t *s, uint64_t seed) {
    log_clear();
    stats_clear();
    g_last_seed = seed;

    log_pushf(CP_HEADER, A_BOLD, "starting %s   seed=%llu", s->id, (unsigned long long)seed);
    redraw_all(running_status);

    pg_run_handle_t h;
    if (pg_run_start(s, seed, &h) < 0) {
        log_pushf(CP_FAULT, A_BOLD, "failed to start scenario");
        redraw_all(running_status);
        return -1;
    }

    struct pollfd pfds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = h.event_fd,   .events = POLLIN },
    };

    int done = 0, aborted = 0;
    while (!done) {
        int pr = poll(pfds, 2, 100);
        if (pr < 0) break;

        if (pfds[0].revents & POLLIN) {
            int ch = getch();
            if ((ch == 'q' || ch == 'Q') && !aborted) {
                log_pushf(CP_FAULT, A_BOLD, "(user aborted -- SIGTERM to child)");
                pg_run_abort(&h);
                aborted = 1;
            }
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            pg_event_t ev;
            int r = pg_run_poll(&h, &ev, 0);
            if (r == 1) {
                push_event(&ev);
                switch (ev.kind) {
                case PG_EV_COUNTER: stat_upsert(ev.key, ev.num, PG_EV_COUNTER, /*delta=*/1); break;
                case PG_EV_GAUGE:   stat_upsert(ev.key, ev.num, PG_EV_GAUGE,   /*delta=*/0); break;
                case PG_EV_EXPECT:  stat_upsert(ev.key, ev.num, PG_EV_EXPECT,  /*delta=*/0); break;
                case PG_EV_ACTUAL:  stat_upsert(ev.key, ev.num, PG_EV_ACTUAL,  /*delta=*/0); break;
                default: break;
                }
                redraw_all(running_status);
                if (ev.kind == PG_EV_DONE) done = 1;
            } else if (r <= 0) {
                done = 1;
            }
        }
    }

    pg_run_reap(&h, &g_last_exit);

    if (WIFSIGNALED(g_last_exit)) {
        log_pushf(CP_FAULT, A_BOLD, "child killed by signal %d", WTERMSIG(g_last_exit));
    } else if (WIFEXITED(g_last_exit) && WEXITSTATUS(g_last_exit) != 0) {
        log_pushf(CP_FAULT, A_BOLD, "child exited %d", WEXITSTATUS(g_last_exit));
    }
    return 0;
}

/* ---------------- layout & main loop ---------------- */

static void layout_windows(void) {
    int rows, cols; getmaxyx(stdscr, rows, cols);
    int status_h = 1;
    int main_h   = rows - status_h;

    int menu_w = cols / 3;
    if (menu_w < 34) menu_w = 34;
    if (menu_w > cols - 24) menu_w = cols - 24;
    if (menu_w < 20) menu_w = 20;

    int right_w = cols - menu_w;
    int log_h   = main_h * 2 / 3;
    int stats_h = main_h - log_h;

    if (win_menu)   delwin(win_menu);
    if (win_log)    delwin(win_log);
    if (win_stats)  delwin(win_stats);
    if (win_status) delwin(win_status);

    win_menu   = newwin(main_h,  menu_w,  0,      0);
    win_log    = newwin(log_h,   right_w, 0,      menu_w);
    win_stats  = newwin(stats_h, right_w, log_h,  menu_w);
    win_status = newwin(status_h, cols,   main_h, 0);
}

static const char *menu_status =
    "[UP/DOWN] select   [ENTER] run   [q] quit";
static const char *post_status =
    "[r] rerun   [R] new seed   [e] lesson   [ENTER] menu   [q] quit";

int tui_run(void) {
    g_scens = pg_registry_list(&g_scen_n);

    if (!initscr()) {
        fprintf(stderr, "ncurses initscr() failed (is this a terminal?)\n");
        return 1;
    }
    cbreak(); noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    init_colors();

    layout_windows();

    enum { S_MENU, S_POST } state = S_MENU;
    int  running = 1;
    const pg_scenario_t *last = NULL;

    redraw_all(menu_status);

    while (running) {
        int ch = getch();
        if (ch == ERR) { napms(40); continue; }

        if (state == S_MENU) {
            switch (ch) {
            case 'q': case 'Q':
                running = 0;
                break;
            case KEY_UP:
                if (g_sel > 0) g_sel--;
                break;
            case KEY_DOWN:
                if ((size_t)g_sel + 1 < g_scen_n) g_sel++;
                break;
            case KEY_RESIZE:
                layout_windows();
                touch_all();
                break;
            case '\n': case KEY_ENTER: case ' ':
                if (g_scen_n > 0) {
                    last = g_scens[g_sel];
                    run_scenario(last, (uint64_t)time(NULL));
                    state = S_POST;
                }
                break;
            default: break;
            }
        } else { /* S_POST */
            switch (ch) {
            case 'q': case 'Q':
                running = 0;
                break;
            case 'r':
                if (last) run_scenario(last, g_last_seed);
                break;
            case 'R':
                if (last) run_scenario(last, (uint64_t)time(NULL) ^ g_last_seed ^ 0x5A5A5A5AULL);
                break;
            case 'e': case 'E':
                if (last) {
                    char title[128];
                    snprintf(title, sizeof(title), "Lesson: %s", last->title);
                    show_overlay(title, last->lesson);
                }
                break;
            case KEY_RESIZE:
                layout_windows();
                touch_all();
                break;
            case '\n': case KEY_ENTER: case ' ': case 27 /* ESC */:
                state = S_MENU;
                break;
            default: break;
            }
        }
        redraw_all(state == S_MENU ? menu_status : post_status);
    }

    if (win_menu)   delwin(win_menu);
    if (win_log)    delwin(win_log);
    if (win_stats)  delwin(win_stats);
    if (win_status) delwin(win_status);
    endwin();
    return 0;
}
