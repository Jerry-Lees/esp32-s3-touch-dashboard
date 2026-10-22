/*
 * Servers app: the server monitor's results (engine: monitor.c).
 *
 * - Top: summary (up / partly up / down, when last checked), Check now, menu.
 * - Group tabs, then a grid of cards, one per host: colour strip (green up,
 *   amber partly up, red down, grey not checked yet), name, what it is,
 *   checks up / total and the answer time or the problem. Problems first
 *   unless switched off in the menu.
 * - Tap a card for every check: what it tests, state, time, since when,
 *   and two history charts below them (status blocks and blue answer-time
 *   bars, one per 5 minutes, last 24 h; zoomed to 2/6/12 h while there's
 *   less data; monitor.c keeps the history and saves it to the card).
 * - Edit (top bar, or a server's page): add, change, reorder and delete
 *   servers, checks and groups; Save writes monitors.json on the card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "app_framework.h"
#include "game_common.h"
#include "launcher.h"
#include "monitor.h"
#include "net_time.h"
#include "settings.h"

#define AREA_W    800
#define AREA_H    416
#define TOP_H     50
#define TABS_H    46
#define CARD_W    188
#define CARD_H    80
#define C_PANEL   0x1E262E
#define C_BUTTON  0x2A333C
#define C_ACCENT  0x42A5F5
#define C_UP      0x43A047
#define C_PART    0xF9A825
#define C_DOWN    0xE53935
#define C_UNK     0x5F6B77

static struct {
    lv_obj_t   *root, *summary, *tabs, *grid, *detail, *dbody, *menu;
    lv_timer_t *timer;
    int         group;             /* -1 = all */
    int         detail_host;
    uint32_t    version;
    int         shown_age;
} U;

/* ======================= helpers ======================= */

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* One line, cut with "..." when too long. */
static void one_line(lv_obj_t *l, int w)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, w, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

static lv_obj_t *button(lv_obj_t *p, const char *text, int w, int h, uint32_t color,
                        lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_center(label(b, APP_FONT_MEDIUM, APP_COLOR_TEXT, text));
    return b;
}

static lv_obj_t *overlay(void)
{
    lv_obj_t *o = lv_obj_create(U.root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, AREA_W, AREA_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static uint32_t state_color(mon_state_t st, bool partial)
{
    return st == MON_DOWN ? C_DOWN : partial ? C_PART : st == MON_UP ? C_UP : C_UNK;
}

/* 0 = down, 1 = partly, 2 = not checked, 3 = up: the "problems first" order. */
static int rank(const mon_host_t *h)
{
    bool partial;
    mon_state_t st = monitor_host_state(h, &partial);
    return st == MON_DOWN ? 0 : partial ? 1 : st == MON_UNKNOWN ? 2 : 3;
}

static void fmt_since(char *out, size_t len, uint32_t up)
{
    if (!up) {
        snprintf(out, len, "-");
        return;
    }
    uint32_t ago = monitor_now() - up;
    if (!net_time_is_synced()) {               /* no clock: say how long ago */
        if (ago < 3600) snprintf(out, len, "for %lu min", (unsigned long)(ago / 60));
        else snprintf(out, len, "for %lu h", (unsigned long)(ago / 3600));
        return;
    }
    time_t n = time(NULL), t = n - ago;
    struct tm tm, now;
    localtime_r(&t, &tm);
    localtime_r(&n, &now);
    bool h24 = settings_get_24h();
    char hm[16];
    strftime(hm, sizeof(hm), h24 ? "%H:%M" : "%I:%M %p", &tm);
    if (!h24 && hm[0] == '0') memmove(hm, hm + 1, strlen(hm));
    if (tm.tm_yday == now.tm_yday && tm.tm_year == now.tm_year) {
        snprintf(out, len, "since %s", hm);
    } else {
        char d[16];
        strftime(d, sizeof(d), "%b %d", &tm);
        snprintf(out, len, "since %s %s", d, hm);
    }
}

/* ======================= summary ======================= */

static void update_summary(void)
{
    int up = 0, part = 0, down = 0, unk = 0;
    char problem[96];
    monitor_lock();
    const mon_cfg_t *c = monitor_cfg();
    for (int i = 0; i < c->nhosts; i++) {
        int r = rank(&c->h[i]);
        if (r == 0) down++; else if (r == 1) part++; else if (r == 2) unk++; else up++;
    }
    snprintf(problem, sizeof(problem), "%s", monitor_problem());
    int nhosts = c->nhosts;
    monitor_unlock();

    char buf[256];
    int n = 0;
    if (nhosts) {
        n += snprintf(buf + n, sizeof(buf) - n, "#66BB6A %d up#", up);
        if (part) n += snprintf(buf + n, sizeof(buf) - n, "   #FFCA28 %d partly#", part);
        if (down) n += snprintf(buf + n, sizeof(buf) - n, "   #EF5350 %d down#", down);
        if (unk) n += snprintf(buf + n, sizeof(buf) - n, "   #9AA5B1 %d waiting#", unk);
    }
    uint32_t last = monitor_last_round();
    if (problem[0] && nhosts) {
        n += snprintf(buf + n, sizeof(buf) - n, "   #FFB74D %s#", problem);
    } else if (last) {
        int age = (int)(monitor_now() - last);
        if (age < 5) n += snprintf(buf + n, sizeof(buf) - n, "   #9AA5B1 checked just now#");
        else if (age < 120) n += snprintf(buf + n, sizeof(buf) - n, "   #9AA5B1 checked %d s ago#", age);
        else n += snprintf(buf + n, sizeof(buf) - n, "   #9AA5B1 checked %d min ago#", age / 60);
    } else if (nhosts) {
        n += snprintf(buf + n, sizeof(buf) - n, "   #9AA5B1 checking...#");
    }
    lv_label_set_text(U.summary, n ? buf : "");
}

/* ======================= cards ======================= */

static void open_detail(int host);
static void edit_open(int host);

static void card_clicked(lv_event_t *e)
{
    open_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void make_card(const mon_host_t *h, int index)
{
    bool partial;
    mon_state_t st = monitor_host_state(h, &partial);
    uint32_t col = state_color(st, partial);

    lv_obj_t *c = lv_obj_create(U.grid);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, CARD_W, CARD_H);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x2A3540), LV_STATE_PRESSED);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *strip = lv_obj_create(c);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 7, CARD_H);
    lv_obj_set_style_bg_color(strip, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = label(c, APP_FONT_MEDIUM, APP_COLOR_TEXT, h->name);
    one_line(n, CARD_W - 22);
    lv_obj_set_pos(n, 15, 6);

    lv_obj_t *w = label(c, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, h->what[0] ? h->what : h->addr);
    one_line(w, CARD_W - 22);
    lv_obj_set_pos(w, 15, 32);

    /* Bottom line: "3/3  4 ms", or the first failing check and why. */
    int up = 0, ms = -1;
    const mon_check_t *bad = NULL;
    for (int i = 0; i < h->ncheck; i++) {
        const mon_check_t *k = &h->c[i];
        if (k->state == MON_UP) {
            up++;
            if (k->kind == MON_PING || ms < 0) ms = k->ms;
        } else if (k->state == MON_DOWN && !bad) {
            bad = k;
        }
    }
    char line[64];
    uint32_t lc = APP_COLOR_SUBTEXT;
    if (bad) {
        snprintf(line, sizeof(line), "%s: %s", bad->label, bad->err[0] ? bad->err : "down");
        lc = st == MON_DOWN ? 0xEF5350 : 0xFFCA28;
    } else if (st == MON_UNKNOWN) {
        snprintf(line, sizeof(line), "Not checked yet");
    } else if (ms >= 0) {
        snprintf(line, sizeof(line), "%d/%d up   %d ms", up, h->ncheck, ms);
        lc = 0x81C784;
    } else {
        snprintf(line, sizeof(line), "%d/%d up", up, h->ncheck);
        lc = 0x81C784;
    }
    lv_obj_t *s = label(c, LV_FONT_DEFAULT, lc, line);
    one_line(s, CARD_W - 22);
    lv_obj_set_pos(s, 15, 54);
}

static int cmp_rank(const void *a, const void *b)
{
    const mon_cfg_t *c = monitor_cfg();
    int ia = *(const int *)a, ib = *(const int *)b;
    int ra = rank(&c->h[ia]), rb = rank(&c->h[ib]);
    return ra != rb ? ra - rb : ia - ib;
}

static void rebuild_cards(void)
{
    lv_coord_t sy = lv_obj_get_scroll_y(U.grid);
    lv_obj_clean(U.grid);
    monitor_lock();
    const mon_cfg_t *c = monitor_cfg();
    if (U.group >= c->ngroups) U.group = -1;
    static int order[MON_HOSTS];
    int n = 0;
    for (int i = 0; i < c->nhosts; i++) {
        if (U.group < 0 || c->h[i].group == U.group) order[n++] = i;
    }
    if (monitor_opt("mon_sort")) qsort(order, n, sizeof(int), cmp_rank);
    for (int i = 0; i < n; i++) make_card(&c->h[order[i]], order[i]);
    const char *problem = c->nhosts ? NULL : monitor_problem();
    monitor_unlock();
    if (problem) {
        lv_obj_t *l = label(U.grid, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, problem);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, AREA_W - 60);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(l, 60, 0);
    }
    lv_obj_update_layout(U.grid);
    lv_obj_scroll_to_y(U.grid, sy, LV_ANIM_OFF);
}

/* ======================= group tabs ======================= */

static void tab_clicked(lv_event_t *e)
{
    U.group = (int)(intptr_t)lv_event_get_user_data(e);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(U.tabs); i++) {
        lv_obj_t *t = lv_obj_get_child(U.tabs, i);
        bool on = lv_obj_get_user_data(t) == (void *)(intptr_t)U.group;
        lv_obj_set_style_bg_color(t, lv_color_hex(on ? C_ACCENT : C_BUTTON), 0);
    }
    rebuild_cards();
}

static void rebuild_tabs(void)
{
    lv_obj_clean(U.tabs);
    monitor_lock();
    const mon_cfg_t *c = monitor_cfg();
    int ng = c->nhosts ? c->ngroups : 0;
    char names[MON_GROUPS][24];
    for (int g = 0; g < ng; g++) memcpy(names[g], c->group[g], sizeof(names[g]));
    monitor_unlock();
    if (ng < 2) {
        U.group = -1;
        return;
    }
    for (int g = -1; g < ng; g++) {
        lv_obj_t *b = lv_btn_create(U.tabs);
        lv_obj_set_height(b, 38);
        lv_obj_set_style_pad_hor(b, 16, 0);
        lv_obj_set_style_radius(b, 19, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(g == U.group ? C_ACCENT : C_BUTTON), 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)g);
        lv_obj_add_event_cb(b, tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)g);
        lv_obj_center(label(b, APP_FONT_MEDIUM, APP_COLOR_TEXT, g < 0 ? "All" : names[g]));
    }
}

/* ======================= detail ======================= */

static void detail_close(lv_event_t *e)
{
    (void)e;
    if (U.detail) lv_obj_del_async(U.detail);
    U.detail = U.dbody = NULL;
    U.detail_host = -1;
}

static void detail_edit(lv_event_t *e)
{
    int host = U.detail_host;
    detail_close(e);
    edit_open(host);
}

/* ---------- history graphs (on the detail page) ---------- */

static mon_hist_t s_hist[MON_HIST_N] EXT_RAM_BSS_ATTR;   /* the open host's, oldest first */
static int        s_hist_scale;                          /* ms at the top of the chart */
static int        s_hist_from;                           /* first slot shown (zoom) */

static uint32_t hist_color(const mon_hist_t *b)
{
    return !b->down ? C_UP : !b->up ? C_DOWN : C_PART;
}

/* Left edge of slot k in a box showing slots s_hist_from.. (MON_HIST_N =
 * just past the right edge). */
static lv_coord_t slot_x(const lv_area_t *a, int k)
{
    return a->x1 + (k - s_hist_from) * lv_area_get_width(a) / (MON_HIST_N - s_hist_from);
}

/* Status chart: one block per 5 minutes in the state's colour (green all
 * checks passed, amber some failed, red none answered); no data = nothing.
 * Neighbouring blocks of the same colour are drawn as one rectangle. */
static void status_draw(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    for (int k = s_hist_from; k < MON_HIST_N;) {
        const mon_hist_t *b = &s_hist[k];
        int j = k + 1;
        if (!b->up && !b->down) {
            k = j;
            continue;
        }
        uint32_t c = hist_color(b);
        while (j < MON_HIST_N && (s_hist[j].up || s_hist[j].down) && hist_color(&s_hist[j]) == c) j++;
        lv_area_t r = { slot_x(&a, k), a.y1, slot_x(&a, j) - 1, a.y2 };
        d.bg_color = lv_color_hex(c);
        lv_draw_rect(ctx, &d, &r);
        k = j;
    }
}

/* Answer-time chart: a blue bar per 5 minutes, height = the slowest answer
 * (0 at the bottom, s_hist_scale at the top); a red mark where nothing
 * answered. */
static void answer_draw(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    int h = lv_area_get_height(&a);

    d.bg_color = lv_color_hex(0x3A4652);                  /* top and half-way lines */
    lv_area_t g = { a.x1, a.y1, a.x2, a.y1 };
    lv_draw_rect(ctx, &d, &g);
    g.y1 = g.y2 = a.y1 + h / 2;
    lv_draw_rect(ctx, &d, &g);

    for (int k = s_hist_from; k < MON_HIST_N; k++) {
        const mon_hist_t *b = &s_hist[k];
        lv_area_t r = { slot_x(&a, k), a.y1, LV_MAX(slot_x(&a, k + 1) - 2, slot_x(&a, k)), a.y2 };
        if (b->ms && s_hist_scale) {
            r.y1 = a.y2 - LV_MAX(2, LV_MIN(h, (b->ms - 1) * h / s_hist_scale)) + 1;
            d.bg_color = lv_color_hex(C_ACCENT);
        } else if (b->down && !b->up) {
            r.y1 = a.y2 - 3;
            d.bg_color = lv_color_hex(C_DOWN);
        } else {
            continue;
        }
        lv_draw_rect(ctx, &d, &r);
    }
}

#define GUTTER  60                       /* left of the charts: the scale */

static lv_obj_t *graph_box(lv_obj_t *p, int y, int h, lv_event_cb_t draw)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, AREA_W - 40 - GUTTER, h);
    lv_obj_set_pos(o, GUTTER, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, draw, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

/* "6 h ago ... 3 h ago ... now" under a chart. */
static void time_axis(lv_obj_t *p, int y, int zoom)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%d h ago", zoom);
    lv_obj_set_pos(label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf), GUTTER, y);
    snprintf(buf, sizeof(buf), "%d h ago", zoom / 2);
    lv_obj_t *l = label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
    lv_obj_align(l, LV_ALIGN_TOP_MID, GUTTER / 2, y);
    lv_obj_align(label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "now"), LV_ALIGN_TOP_RIGHT, 0, y);
}

/* A colour square and its meaning; returns the x after it. */
static int legend(lv_obj_t *p, int x, int y, uint32_t color, const char *text)
{
    lv_obj_t *sq = lv_obj_create(p);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 14, 14);
    lv_obj_set_pos(sq, x, y + 2);
    lv_obj_set_style_radius(sq, 3, 0);
    lv_obj_set_style_bg_color(sq, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_clear_flag(sq, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, text);
    lv_obj_set_pos(l, x + 20, y);
    lv_obj_update_layout(l);
    return x + 20 + lv_obj_get_width(l) + 24;
}

/* The two history charts from s_hist, from y in the detail body. */
static void add_history(int y)
{
    /* Zoom: the shortest of 2, 6, 12 or 24 hours that holds all the data. */
    static const int hours[] = { 2, 6, 12, 24 };
    int first = MON_HIST_N, zoom = 24;
    for (int k = 0; k < MON_HIST_N && first == MON_HIST_N; k++) {
        if (s_hist[k].up || s_hist[k].down) first = k;
    }
    for (int i = 0; i < 4; i++) {
        zoom = hours[i];
        if (MON_HIST_N - first <= zoom * 3600 / MON_HIST_SEC) break;
    }
    s_hist_from = MON_HIST_N - zoom * 3600 / MON_HIST_SEC;

    int n = 0, ok = 0, max = 0, sum = 0, cnt = 0;
    for (int k = s_hist_from; k < MON_HIST_N; k++) {
        const mon_hist_t *b = &s_hist[k];
        if (!b->up && !b->down) continue;
        n++;
        ok += !b->down;
        if (b->ms) {
            max = LV_MAX(max, b->ms - 1);
            sum += b->ms - 1;
            cnt++;
        }
    }
    static const int steps[] = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 65535 };
    s_hist_scale = steps[0];
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        s_hist_scale = steps[i];
        if (steps[i] >= max) break;
    }

    lv_obj_t *p = U.dbody, *l;
    char buf[128];

    /* ---- status ---- */
    snprintf(buf, sizeof(buf), "Status, last %d hours", zoom);
    lv_obj_set_pos(label(p, APP_FONT_MEDIUM, APP_COLOR_TEXT, buf), 0, y);
    if (n) {
        int pct = ok * 1000 / n;                         /* tenths of a percent */
        snprintf(buf, sizeof(buf), "all checks up %d.%d%% of the time", pct / 10, pct % 10);
    } else {
        snprintf(buf, sizeof(buf), "fills in as checks run (one block per 5 minutes)");
    }
    l = label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, y + 4);
    int x = legend(p, GUTTER, y + 28, C_UP, "all checks up");
    x = legend(p, x, y + 28, C_PART, "some checks down");
    legend(p, x, y + 28, C_DOWN, "no check answered");
    graph_box(p, y + 50, 26, status_draw);
    time_axis(p, y + 78, zoom);

    /* ---- answer time ---- */
    y += 112;
    lv_obj_set_pos(label(p, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Answer time"), 0, y);
    if (cnt) snprintf(buf, sizeof(buf), "slowest answer in each 5 minutes   " LV_SYMBOL_BULLET
                      "   average %d ms", sum / cnt);
    else snprintf(buf, sizeof(buf), "slowest answer in each 5 minutes");
    l = label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
    lv_obj_align(l, LV_ALIGN_TOP_RIGHT, 0, y + 4);
    graph_box(p, y + 30, 80, answer_draw);
    /* Scale on the left: top, half-way, 0. */
    static const int at[] = { 0, 40, 80 };
    for (int i = 0; i < 3; i++) {
        int v = s_hist_scale * (2 - i) / 2;
        if (v >= 1000 && v % 1000 == 0) snprintf(buf, sizeof(buf), "%d s", v / 1000);
        else snprintf(buf, sizeof(buf), "%d ms", v);
        l = label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
        lv_obj_set_width(l, GUTTER - 8);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(l, 0, y + 30 + at[i] - 8);
    }
    time_axis(p, y + 114, zoom);

    lv_obj_t *pad = lv_obj_create(p);                  /* bottom margin when scrolled down */
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 10, 12);
    lv_obj_set_pos(pad, 0, y + 132);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
}

static void fill_detail(void)
{
    lv_coord_t sy = lv_obj_get_scroll_y(U.dbody);
    lv_obj_clean(U.dbody);
    monitor_lock();
    const mon_cfg_t *cfg = monitor_cfg();
    if (U.detail_host >= cfg->nhosts) {
        monitor_unlock();
        return;
    }
    const mon_host_t *h = &cfg->h[U.detail_host];
    char buf[160];
    snprintf(buf, sizeof(buf), "%s   " LV_SYMBOL_BULLET "   %s", h->addr, cfg->group[h->group]);
    lv_obj_t *l = label(U.dbody, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
    lv_obj_set_pos(l, 0, 0);
    if (h->what[0]) {
        l = label(U.dbody, APP_FONT_MEDIUM, APP_COLOR_TEXT, h->what);
        one_line(l, AREA_W - 40);
        lv_obj_set_pos(l, 0, 24);
    }
    for (int i = 0; i < h->ncheck; i++) {
        const mon_check_t *k = &h->c[i];
        int y = 58 + i * 52;
        lv_obj_t *row = lv_obj_create(U.dbody);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, AREA_W - 40, 46);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 16, 16);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(state_color(k->state, false)), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 14, 0);

        l = label(row, APP_FONT_MEDIUM, APP_COLOR_TEXT, k->label);
        lv_obj_set_pos(l, 42, 2);
        if (k->kind == MON_PING) snprintf(buf, sizeof(buf), "Ping (ICMP echo)");
        else if (k->kind == MON_TCP) snprintf(buf, sizeof(buf), "TCP port %u", k->port);
        else snprintf(buf, sizeof(buf), "%s", k->url);
        l = label(row, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
        one_line(l, 400);
        lv_obj_set_pos(l, 42, 26);

        char since[40];
        fmt_since(since, sizeof(since), k->since);
        uint32_t col = APP_COLOR_SUBTEXT;
        if (k->state == MON_UP) {
            if (k->ms >= 0) snprintf(buf, sizeof(buf), "Up   %d ms", k->ms);
            else snprintf(buf, sizeof(buf), "Up");
            col = 0x81C784;
        } else if (k->state == MON_DOWN) {
            snprintf(buf, sizeof(buf), "Down   %s", k->err[0] ? k->err : "");
            col = 0xEF5350;
        } else {
            snprintf(buf, sizeof(buf), "Not checked yet");
        }
        if (k->state == MON_UP && k->fails) {
            /* Up, but the last check failed: say so (two in a row = down). */
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "   (last: %s)", k->err);
            col = 0xFFCA28;
        }
        l = label(row, APP_FONT_MEDIUM, col, buf);
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -14, 2);
        l = label(row, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, since);
        lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, -14, -3);
    }
    int y = 58 + h->ncheck * 52 + 4;
    monitor_history(U.detail_host, s_hist);
    monitor_unlock();
    add_history(y);
    lv_obj_update_layout(U.dbody);
    lv_obj_scroll_to_y(U.dbody, sy, LV_ANIM_OFF);
}

static void open_detail(int host)
{
    if (U.detail) return;
    U.detail_host = host;
    U.detail = overlay();
    monitor_lock();
    char name[MON_NAME];
    snprintf(name, sizeof(name), "%s", host < monitor_cfg()->nhosts ? monitor_cfg()->h[host].name : "");
    monitor_unlock();
    lv_obj_set_pos(label(U.detail, APP_FONT_LARGE, APP_COLOR_TEXT, name), 20, 12);
    lv_obj_align(button(U.detail, LV_SYMBOL_CLOSE "  Close", 120, 44, C_BUTTON, detail_close, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_align(button(U.detail, LV_SYMBOL_EDIT "  Edit", 110, 44, C_BUTTON, detail_edit, NULL),
                 LV_ALIGN_TOP_RIGHT, -142, 8);
    U.dbody = lv_obj_create(U.detail);
    lv_obj_remove_style_all(U.dbody);
    lv_obj_set_size(U.dbody, AREA_W - 28, AREA_H - 66);
    lv_obj_set_style_pad_right(U.dbody, 12, 0);              /* the scrollbar's lane */
    lv_obj_set_pos(U.dbody, 20, 60);
    /* Clickable, or swipes over its (non-clickable) contents reach the page
     * behind, which doesn't scroll. */
    lv_obj_add_flag(U.dbody, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(U.dbody, LV_DIR_VER);           /* 4 checks push the chart down */
    lv_obj_set_style_bg_color(U.dbody, lv_color_hex(0x5F6B77), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(U.dbody, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(U.dbody, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(U.dbody, 3, LV_PART_SCROLLBAR);
    lv_obj_set_scrollbar_mode(U.dbody, LV_SCROLLBAR_MODE_AUTO);
    fill_detail();
}

/* ======================= menu ======================= */

static void menu_close(lv_event_t *e)
{
    (void)e;
    if (U.menu) lv_obj_del_async(U.menu);
    U.menu = NULL;
}

static void reload_clicked(lv_event_t *e)
{
    monitor_reload();
    launcher_toast("Reading monitors.json from the SD card...", 2500);
    menu_close(e);
}

static void opt_changed(lv_event_t *e)
{
    const char *key = lv_event_get_user_data(e);
    monitor_set_opt(key, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    if (!strcmp(key, "mon_sort")) rebuild_cards();
}

static lv_obj_t *checkbox(lv_obj_t *p, const char *text, const char *key)
{
    lv_obj_t *c = lv_checkbox_create(p);
    lv_checkbox_set_text(c, text);
    lv_obj_set_style_text_font(c, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_set_style_pad_column(c, 12, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_BUTTON), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(c, lv_color_hex(0x5F6B77), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(c, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (monitor_opt(key)) lv_obj_add_state(c, LV_STATE_CHECKED);
    lv_obj_add_event_cb(c, opt_changed, LV_EVENT_VALUE_CHANGED, (void *)key);
    return c;
}

static void menu_clicked(lv_event_t *e)
{
    (void)e;
    if (U.menu) return;
    lv_obj_t *o = U.menu = overlay();
    lv_obj_set_pos(label(o, APP_FONT_LARGE, APP_COLOR_TEXT, "Servers"), 20, 12);
    lv_obj_align(button(o, LV_SYMBOL_CLOSE "  Close", 120, 44, C_BUTTON, menu_close, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_set_pos(checkbox(o, "Problems first", "mon_sort"), 20, 76);
    lv_obj_set_pos(checkbox(o, "Show in the home screen's info ticker", "mon_tk"), 20, 120);
    lv_obj_set_pos(checkbox(o, "Pop-up when a server goes down or comes back", "mon_pop"), 20, 164);
    lv_obj_t *b = button(o, LV_SYMBOL_REFRESH "  Reload the list from the SD card", 420, 50,
                         C_BUTTON, reload_clicked, NULL);
    lv_obj_set_pos(b, 20, 214);
    lv_obj_t *l = label(o, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                        "The list is dashboard/monitors.json on the SD card. Change it here with "
                        LV_SYMBOL_EDIT " (top bar), or on the PC (format in the README) and copy it "
                        "with: tools/sdcard.py put --force monitors.json monitors.json\n"
                        "It's re-read automatically when it changes. Saving here keeps the old "
                        "file as monitors.json.bak. A check is down after two failures in a row.");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, AREA_W - 40);
    lv_obj_set_pos(l, 20, 284);
}

/* ======================= list editor ======================= */
/*
 * Edit servers: changes go to a working copy of the list (E.cfg, PSRAM);
 * nothing happens until Save, which writes monitors.json via monitor_save().
 * Main page: groups with their servers (tap one for its form, arrows to
 * reorder). The server form has name, address, description, group and up
 * to MON_CHECKS checks. Rebuilds triggered from widgets inside the page
 * being rebuilt go through lv_async_call.
 */
#define KB_H      220
#define FORM_Y    60
#define C_SAVE    0x1B8F4A
#define C_DANGER  0x8E2A2A

static const int s_intervals[] = { 15, 30, 60, 120, 300, 600, 1800, 3600 };
#define N_INTERVALS ((int)(sizeof(s_intervals) / sizeof(s_intervals[0])))

static mon_host_t s_draft EXT_RAM_BSS_ATTR;    /* the server form's contents */

static struct {
    mon_cfg_t  *cfg;               /* working copy, NULL = editor closed */
    bool        dirty;
    lv_obj_t   *view, *list;
    lv_obj_t   *form, *fbody, *kb, *focus;
    int         host;              /* server in the form, -1 = new */
    lv_obj_t   *ta_name, *ta_addr, *ta_what, *dd_group;
    lv_obj_t   *ta_arg[MON_CHECKS], *ta_label[MON_CHECKS];
    lv_obj_t   *ask, *ask_ta;
    int         ask_group;         /* group being renamed, -1 = new group */
    int         del_group;
} E;

static void list_fill(void);
static void form_fill(void);

static void list_fill_async(void *p) { (void)p; if (E.list) list_fill(); }
static void form_fill_async(void *p) { (void)p; if (E.fbody) form_fill(); }

/* Copies src without leading/trailing spaces. */
static void trim_to(char *dst, size_t n, const char *src)
{
    while (*src == ' ') src++;
    size_t l = strlen(src);
    while (l && src[l - 1] == ' ') l--;
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static void style_dd(lv_obj_t *dd)
{
    lv_obj_set_style_text_font(dd, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x3A4652), 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_text_font(list, APP_FONT_MEDIUM, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(APP_COLOR_PANEL), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(APP_COLOR_TEXT), 0);
        lv_obj_set_style_max_height(list, 300, 0);
    }
}

static int group_size(int g)
{
    int n = 0;
    for (int i = 0; i < E.cfg->nhosts; i++) n += E.cfg->h[i].group == g;
    return n;
}

/* ---------- keyboard (shared by the form and the name prompt) ---------- */

static void kb_hide(void)
{
    if (!E.kb) return;
    lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    if (E.focus) lv_obj_clear_state(E.focus, LV_STATE_FOCUSED);
    E.focus = NULL;
    if (E.fbody) lv_obj_set_height(E.fbody, AREA_H - FORM_Y);
}

static void ta_clicked(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (E.focus && E.focus != ta) lv_obj_clear_state(E.focus, LV_STATE_FOCUSED);
    E.focus = ta;
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(E.kb, ta);
    lv_keyboard_set_mode(E.kb, lv_obj_get_user_data(ta) ? LV_KEYBOARD_MODE_NUMBER
                                                        : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    if (E.fbody && lv_obj_get_parent(ta) == E.fbody) {
        lv_obj_set_height(E.fbody, AREA_H - FORM_Y - KB_H);
        lv_obj_update_layout(E.fbody);
        lv_obj_scroll_to_view(ta, LV_ANIM_ON);
    }
}

static lv_obj_t *mk_ta(lv_obj_t *p, const char *placeholder, int x, int y, int w, int max,
                       const char *text)
{
    lv_obj_t *ta = lv_textarea_create(p);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_text(ta, text);
    lv_obj_set_style_text_font(ta, APP_FONT_MEDIUM, 0);
    lv_obj_set_size(ta, w, 46);
    lv_obj_set_pos(ta, x, y);
    lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta, ta_clicked, LV_EVENT_CLICKED, NULL);
    return ta;
}

static lv_obj_t *mk_kb(lv_obj_t *p, lv_event_cb_t cb)
{
    lv_obj_t *kb = lv_keyboard_create(p);
    lv_obj_set_size(kb, AREA_W, KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, cb, LV_EVENT_CANCEL, NULL);
    lv_keyboard_set_textarea(kb, NULL);
    return kb;
}

/* ---------- group name prompt ---------- */

static void ask_close(void)
{
    if (E.ask) lv_obj_del_async(E.ask);
    E.ask = E.ask_ta = E.kb = E.focus = NULL;
}

static void ask_ok(void)
{
    char name[sizeof(E.cfg->group[0])];
    trim_to(name, sizeof(name), lv_textarea_get_text(E.ask_ta));
    if (!name[0]) {
        launcher_toast("Type a name for the group.", 2500);
        return;
    }
    for (int g = 0; g < E.cfg->ngroups; g++) {
        if (g != E.ask_group && !strcasecmp(E.cfg->group[g], name)) {
            launcher_toast("There's already a group with that name.", 2500);
            return;
        }
    }
    int g = E.ask_group;
    if (g < 0) {
        if (E.cfg->ngroups >= MON_GROUPS) return;
        g = E.cfg->ngroups++;
    }
    memcpy(E.cfg->group[g], name, sizeof(name));
    E.dirty = true;
    ask_close();
    list_fill();
}

static void ask_kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) ask_ok();
    else ask_close();
}

static void ask_ok_clicked(lv_event_t *e) { (void)e; ask_ok(); }
static void ask_cancel_clicked(lv_event_t *e) { (void)e; ask_close(); }

static void ask_open(int group)
{
    if (E.ask) return;
    if (group < 0 && E.cfg->ngroups >= MON_GROUPS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "The list is full (%d groups).", MON_GROUPS);
        launcher_toast(msg, 3000);
        return;
    }
    E.ask_group = group;
    lv_obj_t *o = E.ask = overlay();
    lv_obj_set_parent(o, E.view);
    lv_obj_set_pos(label(o, APP_FONT_LARGE, APP_COLOR_TEXT, group < 0 ? "New group" : "Rename group"),
                   20, 12);
    lv_obj_align(button(o, LV_SYMBOL_OK "  OK", 110, 44, C_SAVE, ask_ok_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_align(button(o, "Cancel", 116, 44, C_BUTTON, ask_cancel_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -134, 8);
    E.ask_ta = mk_ta(o, "Group name, e.g. Network", 20, 70, AREA_W - 40,
                     sizeof(E.cfg->group[0]) - 1, group < 0 ? "" : E.cfg->group[group]);
    E.kb = mk_kb(o, ask_kb_event);
    lv_event_send(E.ask_ta, LV_EVENT_CLICKED, NULL);
}

/* ---------- server form ---------- */

/* Copies what's typed into s_draft (no checks yet: that's form_done). */
static void form_read(void)
{
    mon_host_t *d = &s_draft;
    snprintf(d->name, sizeof(d->name), "%s", lv_textarea_get_text(E.ta_name));
    snprintf(d->addr, sizeof(d->addr), "%s", lv_textarea_get_text(E.ta_addr));
    snprintf(d->what, sizeof(d->what), "%s", lv_textarea_get_text(E.ta_what));
    if (E.dd_group) d->group = lv_dropdown_get_selected(E.dd_group);
    for (int k = 0; k < d->ncheck; k++) {
        mon_check_t *c = &d->c[k];
        if (E.ta_arg[k] && c->kind == MON_TCP) {
            long p = strtol(lv_textarea_get_text(E.ta_arg[k]), NULL, 10);
            c->port = p >= 1 && p <= 65535 ? (uint16_t)p : 0;
        } else if (E.ta_arg[k]) {
            snprintf(c->url, sizeof(c->url), "%s", lv_textarea_get_text(E.ta_arg[k]));
        }
        if (E.ta_label[k]) snprintf(c->label, sizeof(c->label), "%s", lv_textarea_get_text(E.ta_label[k]));
    }
}

static void form_close(void)
{
    if (E.form) lv_obj_del_async(E.form);
    E.form = E.fbody = E.kb = E.focus = E.ta_name = E.ta_addr = E.ta_what = E.dd_group = NULL;
    memset(E.ta_arg, 0, sizeof(E.ta_arg));
    memset(E.ta_label, 0, sizeof(E.ta_label));
}

static void kind_changed(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    form_read();
    mon_check_t *c = &s_draft.c[k];
    int kind = lv_dropdown_get_selected(lv_event_get_target(e));
    if (kind == c->kind) return;
    c->kind = kind;
    if (kind == MON_HTTP && !c->url[0]) snprintf(c->url, sizeof(c->url), "http://%s/", s_draft.addr);
    lv_async_call(form_fill_async, NULL);
}

static void check_remove(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    form_read();
    mon_host_t *d = &s_draft;
    memmove(&d->c[k], &d->c[k + 1], (d->ncheck - k - 1) * sizeof(d->c[0]));
    d->ncheck--;
    kb_hide();
    lv_async_call(form_fill_async, NULL);
}

static void check_add(lv_event_t *e)
{
    (void)e;
    form_read();
    mon_host_t *d = &s_draft;
    if (d->ncheck >= MON_CHECKS) return;
    bool ping = false;
    for (int k = 0; k < d->ncheck; k++) ping |= d->c[k].kind == MON_PING;
    mon_check_t *c = &d->c[d->ncheck++];
    memset(c, 0, sizeof(*c));
    c->kind = ping ? MON_TCP : MON_PING;        /* a ping first, if there's none */
    kb_hide();
    lv_async_call(form_fill_async, NULL);
}

/* A dialog inside a page that's being closed must not also be deleted on
 * its own: both deletes are async, the newest runs first, so the dialog's
 * would then hit freed memory. Deleting the page deletes the dialog too. */
static void del_host_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    if (!yes || E.host < 0) {
        game_dialog_close();
        return;
    }
    mon_cfg_t *c = E.cfg;
    memmove(&c->h[E.host], &c->h[E.host + 1], (c->nhosts - E.host - 1) * sizeof(c->h[0]));
    c->nhosts--;
    E.dirty = true;
    form_close();
    list_fill();
}

static void host_delete(lv_event_t *e)
{
    (void)e;
    char t[64];
    snprintf(t, sizeof(t), "Delete %s?", E.cfg->h[E.host].name);
    lv_obj_t *row = game_dialog_open(E.form, t);
    game_dialog_button(row, LV_SYMBOL_TRASH "  Delete", del_host_answer, (void *)1);
    game_dialog_button(row, "Cancel", del_host_answer, NULL);
}

static void form_done(lv_event_t *e)
{
    (void)e;
    form_read();
    mon_host_t *d = &s_draft;
    char msg[96] = "";
    trim_to(d->name, sizeof(d->name), d->name);
    trim_to(d->addr, sizeof(d->addr), d->addr);
    trim_to(d->what, sizeof(d->what), d->what);
    if (!d->name[0]) snprintf(msg, sizeof(msg), "Give the server a name.");
    else if (!d->addr[0] || strchr(d->addr, ' '))
        snprintf(msg, sizeof(msg), "Type the address: an IP address or host name, no spaces.");
    else if (!d->ncheck) snprintf(msg, sizeof(msg), "Add at least one check.");
    for (int k = 0; k < d->ncheck && !msg[0]; k++) {
        mon_check_t *c = &d->c[k];
        trim_to(c->label, sizeof(c->label), c->label);
        trim_to(c->url, sizeof(c->url), c->url);
        if (c->kind == MON_PING) {
            snprintf(c->label, sizeof(c->label), "Ping");
            c->port = 0;
            c->url[0] = '\0';
        } else if (c->kind == MON_TCP) {
            c->url[0] = '\0';
            if (!c->port) snprintf(msg, sizeof(msg), "Check %d: the port must be 1 to 65535.", k + 1);
        } else {
            c->port = 0;
            bool scheme = !strncmp(c->url, "http://", 7) || !strncmp(c->url, "https://", 8);
            if (!scheme || strchr(c->url, ' ') || strlen(c->url) < 10) {
                snprintf(msg, sizeof(msg), "Check %d: the web address must start with http:// "
                         "or https://, no spaces.", k + 1);
            }
        }
    }
    if (msg[0]) {
        launcher_toast(msg, 3500);
        return;
    }
    mon_cfg_t *c = E.cfg;
    if (E.host >= 0 && c->h[E.host].group != d->group) {
        /* Another group: it goes to the end of that one. */
        memmove(&c->h[E.host], &c->h[E.host + 1], (c->nhosts - E.host - 1) * sizeof(c->h[0]));
        c->nhosts--;
        E.host = -1;
    }
    if (E.host < 0) {
        if (c->nhosts >= MON_HOSTS) return;
        E.host = c->nhosts++;
    }
    c->h[E.host] = *d;
    E.dirty = true;
    form_close();
    list_fill();
}

static void form_cancel(lv_event_t *e) { (void)e; form_close(); }

static void form_kb_event(lv_event_t *e) { (void)e; kb_hide(); }

static void form_fill(void)
{
    mon_host_t *d = &s_draft;
    lv_obj_clean(E.fbody);
    E.focus = NULL;
    memset(E.ta_arg, 0, sizeof(E.ta_arg));
    memset(E.ta_label, 0, sizeof(E.ta_label));
    lv_obj_t *p = E.fbody;
    static const char *names[] = { "Name", "Address", "Description", "Group" };
    for (int r = 0; r < 4; r++) {
        lv_obj_t *l = label(p, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, names[r]);
        lv_obj_set_pos(l, 0, r * 56 + 12);
    }
    E.ta_name = mk_ta(p, "Short name, e.g. router", 150, 0, 610, MON_NAME - 1, d->name);
    E.ta_addr = mk_ta(p, "IP address or host name, e.g. 192.168.1.1", 150, 56, 610,
                      sizeof(d->addr) - 1, d->addr);
    E.ta_what = mk_ta(p, "Optional, e.g. Home router", 150, 112, 610, sizeof(d->what) - 1, d->what);

    char opts[MON_GROUPS * 26] = "";
    for (int g = 0; g < E.cfg->ngroups; g++) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof(opts) - l, "%s%s", g ? "\n" : "", E.cfg->group[g]);
    }
    E.dd_group = lv_dropdown_create(p);
    lv_dropdown_set_options(E.dd_group, opts);
    lv_dropdown_set_selected(E.dd_group, d->group);
    lv_obj_set_pos(E.dd_group, 150, 168);
    lv_obj_set_width(E.dd_group, 320);
    style_dd(E.dd_group);

    lv_obj_t *l = label(p, APP_FONT_LARGE, APP_COLOR_TEXT, "Checks");
    lv_obj_set_pos(l, 0, 236);
    if (d->ncheck < MON_CHECKS) {
        lv_obj_set_pos(button(p, LV_SYMBOL_PLUS "  Add a check", 190, 42, C_BUTTON, check_add, NULL),
                       150, 230);
    }
    l = label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
              "Ping: answers ping   TCP: the port\naccepts connections   Web: answers HTTP");
    lv_obj_set_pos(l, 360, 234);

    int y = 288;
    for (int k = 0; k < d->ncheck; k++, y += 56) {
        mon_check_t *c = &d->c[k];
        lv_obj_t *dd = lv_dropdown_create(p);
        lv_dropdown_set_options(dd, "Ping\nTCP\nWeb");
        lv_dropdown_set_selected(dd, c->kind);
        lv_obj_set_pos(dd, 0, y);
        lv_obj_set_width(dd, 120);
        style_dd(dd);
        lv_obj_add_event_cb(dd, kind_changed, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        if (c->kind == MON_PING) {
            l = label(p, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "Ping (ICMP echo)");
            lv_obj_set_pos(l, 136, y + 12);
        } else if (c->kind == MON_TCP) {
            char port[8] = "";
            if (c->port) snprintf(port, sizeof(port), "%u", c->port);
            E.ta_arg[k] = mk_ta(p, "Port", 130, y, 120, 5, port);
            lv_textarea_set_accepted_chars(E.ta_arg[k], "0123456789");
            lv_obj_set_user_data(E.ta_arg[k], (void *)1);      /* number keyboard */
            E.ta_label[k] = mk_ta(p, "Label, e.g. SSH (optional)", 260, y, 430,
                                  sizeof(c->label) - 1, c->label);
        } else {
            E.ta_arg[k] = mk_ta(p, "http://...", 130, y, 400, sizeof(c->url) - 1, c->url);
            E.ta_label[k] = mk_ta(p, "Label", 540, y, 150, sizeof(c->label) - 1, c->label);
        }
        lv_obj_set_pos(button(p, LV_SYMBOL_CLOSE, 56, 46, C_DANGER, check_remove, (void *)(intptr_t)k),
                       704, y);
    }
    if (E.host >= 0) {
        lv_obj_set_pos(button(p, LV_SYMBOL_TRASH "  Delete this server", 280, 46, C_DANGER,
                              host_delete, NULL), 0, y + 14);
        y += 60;
    }
    /* Room to scroll the last row above the keyboard. */
    lv_obj_t *pad = lv_obj_create(p);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 10, 20);
    lv_obj_set_pos(pad, 0, y + 10);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_CLICKABLE);
}

static void form_open(int host, int group)
{
    if (E.form) return;
    if (host < 0 && E.cfg->nhosts >= MON_HOSTS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "The list is full (%d servers).", MON_HOSTS);
        launcher_toast(msg, 3000);
        return;
    }
    E.host = host;
    if (host >= 0) {
        s_draft = E.cfg->h[host];
    } else {
        memset(&s_draft, 0, sizeof(s_draft));
        s_draft.group = group;
        s_draft.ncheck = 1;
        s_draft.c[0].kind = MON_PING;
    }
    lv_obj_t *o = E.form = overlay();
    lv_obj_set_parent(o, E.view);
    lv_obj_set_pos(label(o, APP_FONT_LARGE, APP_COLOR_TEXT, host < 0 ? "New server" : "Edit server"),
                   20, 12);
    lv_obj_align(button(o, LV_SYMBOL_OK "  Done", 120, 44, C_SAVE, form_done, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_align(button(o, "Cancel", 116, 44, C_BUTTON, form_cancel, NULL),
                 LV_ALIGN_TOP_RIGHT, -144, 8);
    E.fbody = lv_obj_create(o);
    lv_obj_remove_style_all(E.fbody);
    lv_obj_set_size(E.fbody, AREA_W - 20, AREA_H - FORM_Y);
    lv_obj_set_pos(E.fbody, 20, FORM_Y);
    lv_obj_set_scroll_dir(E.fbody, LV_DIR_VER);
    E.kb = mk_kb(o, form_kb_event);
    lv_obj_add_flag(E.kb, LV_OBJ_FLAG_HIDDEN);
    form_fill();
}

/* ---------- main editor page ---------- */

static void edit_close(void)
{
    if (E.view) lv_obj_del_async(E.view);
    free(E.cfg);
    memset(&E, 0, sizeof(E));
}

static void host_clicked(lv_event_t *e)
{
    form_open((int)(intptr_t)lv_event_get_user_data(e), 0);
}

/* Swaps server i with the one before (dir -1) or after (+1) it in its group. */
static void host_move(int i, int dir)
{
    mon_cfg_t *c = E.cfg;
    for (int j = i + dir; j >= 0 && j < c->nhosts; j += dir) {
        if (c->h[j].group != c->h[i].group) continue;
        mon_host_t t = c->h[i];
        c->h[i] = c->h[j];
        c->h[j] = t;
        E.dirty = true;
        lv_async_call(list_fill_async, NULL);
        return;
    }
}

static void host_up(lv_event_t *e)   { host_move((int)(intptr_t)lv_event_get_user_data(e), -1); }
static void host_down(lv_event_t *e) { host_move((int)(intptr_t)lv_event_get_user_data(e), 1); }

static void group_move(int g, int dir)
{
    mon_cfg_t *c = E.cfg;
    int o = g + dir;
    if (o < 0 || o >= c->ngroups) return;
    char t[sizeof(c->group[0])];
    memcpy(t, c->group[g], sizeof(t));
    memcpy(c->group[g], c->group[o], sizeof(t));
    memcpy(c->group[o], t, sizeof(t));
    for (int i = 0; i < c->nhosts; i++) {
        if (c->h[i].group == g) c->h[i].group = o;
        else if (c->h[i].group == o) c->h[i].group = g;
    }
    E.dirty = true;
    lv_async_call(list_fill_async, NULL);
}

static void group_up(lv_event_t *e)   { group_move((int)(intptr_t)lv_event_get_user_data(e), -1); }
static void group_down(lv_event_t *e) { group_move((int)(intptr_t)lv_event_get_user_data(e), 1); }

static void group_remove(int g)
{
    mon_cfg_t *c = E.cfg;
    int n = 0;
    for (int i = 0; i < c->nhosts; i++) {
        if (c->h[i].group == g) continue;
        c->h[n] = c->h[i];
        if (c->h[n].group > g) c->h[n].group--;
        n++;
    }
    c->nhosts = n;
    memmove(c->group[g], c->group[g + 1], (c->ngroups - g - 1) * sizeof(c->group[0]));
    c->ngroups--;
    E.dirty = true;
    lv_async_call(list_fill_async, NULL);
}

static void del_group_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (yes) group_remove(E.del_group);
}

static void group_delete(lv_event_t *e)
{
    int g = (int)(intptr_t)lv_event_get_user_data(e);
    int n = group_size(g);
    if (!n) {
        group_remove(g);
        return;
    }
    E.del_group = g;
    char t[96];
    snprintf(t, sizeof(t), "Delete %s and its %d server%s?", E.cfg->group[g], n, n == 1 ? "" : "s");
    lv_obj_t *row = game_dialog_open(E.view, t);
    game_dialog_button(row, LV_SYMBOL_TRASH "  Delete", del_group_answer, (void *)1);
    game_dialog_button(row, "Cancel", del_group_answer, NULL);
}

static void group_rename(lv_event_t *e) { ask_open((int)(intptr_t)lv_event_get_user_data(e)); }
static void group_add(lv_event_t *e)    { (void)e; ask_open(-1); }
static void host_add(lv_event_t *e)     { form_open(-1, (int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *row_box(int h)
{
    lv_obj_t *r = lv_obj_create(E.list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, AREA_W - 40, h);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return r;
}

static void list_fill(void)
{
    lv_coord_t sy = lv_obj_get_scroll_y(E.list);
    lv_obj_clean(E.list);
    mon_cfg_t *c = E.cfg;
    if (!c->ngroups) {
        lv_obj_t *l = label(E.list, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT,
                            "No groups yet. Tap \"New group\" to start (e.g. Network or Servers).");
        lv_obj_set_style_pad_top(l, 30, 0);
    }
    for (int g = 0; g < c->ngroups; g++) {
        void *ug = (void *)(intptr_t)g;
        lv_obj_t *r = row_box(48);
        lv_obj_t *l = label(r, APP_FONT_LARGE, C_ACCENT, c->group[g]);
        one_line(l, 300);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *b = button(r, LV_SYMBOL_TRASH, 52, 42, C_DANGER, group_delete, ug);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        b = button(r, LV_SYMBOL_DOWN, 52, 42, C_BUTTON, group_down, ug);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -60, 0);
        b = button(r, LV_SYMBOL_UP, 52, 42, C_BUTTON, group_up, ug);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -120, 0);
        b = button(r, LV_SYMBOL_EDIT "  Rename", 120, 42, C_BUTTON, group_rename, ug);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -180, 0);
        b = button(r, LV_SYMBOL_PLUS "  Server", 120, 42, C_BUTTON, host_add, ug);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -308, 0);

        for (int i = 0; i < c->nhosts; i++) {
            const mon_host_t *h = &c->h[i];
            if (h->group != g) continue;
            void *ui = (void *)(intptr_t)i;
            r = row_box(60);
            lv_obj_set_style_radius(r, 10, 0);
            lv_obj_set_style_bg_color(r, lv_color_hex(C_PANEL), 0);
            lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(r, lv_color_hex(0x2A3540), LV_STATE_PRESSED);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(r, host_clicked, LV_EVENT_CLICKED, ui);
            l = label(r, APP_FONT_MEDIUM, APP_COLOR_TEXT, h->name);
            one_line(l, 580);
            lv_obj_set_pos(l, 14, 6);
            char sub[200];
            int n = snprintf(sub, sizeof(sub), "%s", h->addr);
            if (h->what[0]) n += snprintf(sub + n, sizeof(sub) - n, "   " LV_SYMBOL_BULLET "   %s", h->what);
            for (int k = 0; k < h->ncheck && n < (int)sizeof(sub); k++) {
                const mon_check_t *ck = &h->c[k];
                char port[16];
                snprintf(port, sizeof(port), "port %u", ck->port);
                n += snprintf(sub + n, sizeof(sub) - n, "%s%s", k ? ", " : "   " LV_SYMBOL_BULLET "   ",
                              ck->label[0] ? ck->label : ck->kind == MON_HTTP ? "Web" : port);
            }
            l = label(r, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, sub);
            one_line(l, 580);
            lv_obj_set_pos(l, 14, 34);
            b = button(r, LV_SYMBOL_DOWN, 52, 44, C_BUTTON, host_down, ui);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -8, 0);
            b = button(r, LV_SYMBOL_UP, 52, 44, C_BUTTON, host_up, ui);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -68, 0);
        }
        if (!group_size(g)) {
            l = label(E.list, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "No servers in this group yet.");
            lv_obj_set_style_pad_left(l, 14, 0);
        }
    }
    lv_obj_update_layout(E.list);
    lv_obj_scroll_to_y(E.list, sy, LV_ANIM_OFF);
}

static void interval_changed(lv_event_t *e)
{
    int i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (i < N_INTERVALS) {
        E.cfg->interval = s_intervals[i];
        E.dirty = true;
    }
}

static void save_clicked(lv_event_t *e)
{
    (void)e;
    char why[96];
    if (!monitor_save(E.cfg, why, sizeof(why))) {
        launcher_toast(why, 4000);
        return;
    }
    launcher_toast("Saved to the SD card (the old list is kept as monitors.json.bak). "
                   "The new list starts after the current round of checks.", 4000);
    edit_close();
}

static void discard_answer(lv_event_t *e)
{
    if (lv_event_get_user_data(e)) edit_close();    /* deletes the dialog too */
    else game_dialog_close();
}

static void cancel_clicked(lv_event_t *e)
{
    (void)e;
    if (!E.dirty) {
        edit_close();
        return;
    }
    lv_obj_t *row = game_dialog_open(E.view, "Throw away your changes?");
    game_dialog_button(row, "Throw away", discard_answer, (void *)1);
    game_dialog_button(row, "Keep editing", discard_answer, NULL);
}

/* host >= 0: also open that server's form. */
static void edit_open(int host)
{
    if (E.cfg) return;
    if (monitor_save_pending()) {
        launcher_toast("Still loading the last saved list; try again in a few seconds.", 3000);
        return;
    }
    E.cfg = heap_caps_malloc(sizeof(mon_cfg_t), MALLOC_CAP_SPIRAM);
    if (!E.cfg) {
        launcher_toast("Not enough memory.", 2500);
        return;
    }
    monitor_lock();
    memcpy(E.cfg, monitor_cfg(), sizeof(mon_cfg_t));
    monitor_unlock();
    mon_cfg_t *c = E.cfg;
    if (!c->interval) c->interval = 60;
    if (!c->ngroups) snprintf(c->group[c->ngroups++], sizeof(c->group[0]), "Servers");

    lv_obj_t *o = E.view = overlay();
    lv_obj_set_pos(label(o, APP_FONT_LARGE, APP_COLOR_TEXT, "Edit servers"), 20, 12);
    lv_obj_align(button(o, LV_SYMBOL_SAVE "  Save", 120, 44, C_SAVE, save_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_align(button(o, "Cancel", 116, 44, C_BUTTON, cancel_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -144, 8);

    lv_obj_set_pos(button(o, LV_SYMBOL_PLUS "  New group", 170, 42, C_BUTTON, group_add, NULL), 20, 62);
    lv_obj_t *l = label(o, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "Check every");
    lv_obj_set_pos(l, 214, 72);
    char opts[160] = "15 s\n30 s\n1 min\n2 min\n5 min\n10 min\n30 min\n1 hour";
    int sel = -1;
    for (int i = 0; i < N_INTERVALS; i++) if (s_intervals[i] == c->interval) sel = i;
    if (sel < 0) {                                 /* a value typed into the file */
        size_t n = strlen(opts);
        snprintf(opts + n, sizeof(opts) - n, "\n%d s", c->interval);
        sel = N_INTERVALS;
    }
    lv_obj_t *dd = lv_dropdown_create(o);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_pos(dd, 340, 62);
    lv_obj_set_width(dd, 150);
    style_dd(dd);
    lv_obj_add_event_cb(dd, interval_changed, LV_EVENT_VALUE_CHANGED, NULL);

    E.list = lv_obj_create(o);
    lv_obj_remove_style_all(E.list);
    lv_obj_set_size(E.list, AREA_W, AREA_H - 114);
    lv_obj_set_pos(E.list, 0, 114);
    lv_obj_set_flex_flow(E.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(E.list, 20, 0);
    lv_obj_set_style_pad_bottom(E.list, 12, 0);
    lv_obj_set_style_pad_row(E.list, 8, 0);
    lv_obj_set_scroll_dir(E.list, LV_DIR_VER);
    list_fill();
    if (host >= 0 && host < c->nhosts) form_open(host, 0);
}

static void edit_clicked(lv_event_t *e) { (void)e; edit_open(-1); }

/* ======================= app ======================= */

static void check_now(lv_event_t *e)
{
    (void)e;
    monitor_kick();
    launcher_toast("Checking now...", 1500);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint32_t v = monitor_version();
    if (v != U.version) {
        U.version = v;
        int ng_before = lv_obj_get_child_cnt(U.tabs);
        rebuild_tabs();
        (void)ng_before;
        rebuild_cards();
        if (U.dbody) fill_detail();
        update_summary();
    }
    /* The "checked N s ago" text, once a second. */
    int age = monitor_last_round() ? (int)(monitor_now() - monitor_last_round()) : -1;
    if (age != U.shown_age) {
        U.shown_age = age;
        update_summary();
    }
}

static void monitor_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.root = parent;
    U.group = -1;
    U.detail_host = -1;
    U.shown_age = -2;
    lv_obj_set_style_bg_color(parent, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    U.summary = label(parent, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_recolor(U.summary, true);
    lv_label_set_long_mode(U.summary, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.summary, 500);
    lv_obj_align(U.summary, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_align(button(parent, LV_SYMBOL_REFRESH "  Check", 130, 40, C_BUTTON, check_now, NULL),
                 LV_ALIGN_TOP_RIGHT, -142, 5);
    lv_obj_align(button(parent, LV_SYMBOL_EDIT, 56, 40, C_BUTTON, edit_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -76, 5);
    lv_obj_align(button(parent, LV_SYMBOL_BARS, 56, 40, C_BUTTON, menu_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -10, 5);

    U.tabs = lv_obj_create(parent);
    lv_obj_remove_style_all(U.tabs);
    lv_obj_set_size(U.tabs, AREA_W, TABS_H);
    lv_obj_set_pos(U.tabs, 0, TOP_H);
    lv_obj_set_flex_flow(U.tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(U.tabs, 10, 0);
    lv_obj_set_style_pad_column(U.tabs, 8, 0);
    lv_obj_set_scroll_dir(U.tabs, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(U.tabs, LV_SCROLLBAR_MODE_OFF);

    U.grid = lv_obj_create(parent);
    lv_obj_remove_style_all(U.grid);
    lv_obj_set_size(U.grid, AREA_W, AREA_H - TOP_H - TABS_H);
    lv_obj_set_pos(U.grid, 0, TOP_H + TABS_H);
    lv_obj_set_flex_flow(U.grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(U.grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(U.grid, 8, 0);
    lv_obj_set_style_pad_left(U.grid, 10, 0);
    lv_obj_set_style_pad_gap(U.grid, 8, 0);
    lv_obj_set_scroll_dir(U.grid, LV_DIR_VER);

    U.version = monitor_version();
    rebuild_tabs();
    rebuild_cards();
    update_summary();
    U.timer = lv_timer_create(tick, 250, NULL);
}

static void monitor_destroy(void)
{
    if (U.timer) lv_timer_del(U.timer);
    free(E.cfg);                                 /* unsaved edits are dropped */
    memset(&E, 0, sizeof(E));
    memset(&U, 0, sizeof(U));                    /* the launcher deletes the objects */
}

const app_t monitor_app = {
    .name      = "Servers",
    .symbol    = LV_SYMBOL_LIST,
    .color_hex = 0x2E7D32,
    .create    = monitor_create,
    .destroy   = monitor_destroy,
};
