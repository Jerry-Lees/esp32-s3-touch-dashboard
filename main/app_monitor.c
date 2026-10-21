/*
 * Servers app: the server monitor's results (engine: monitor.c).
 *
 * - Top: summary (up / partly up / down, when last checked), Check now, menu.
 * - Group tabs, then a grid of cards, one per host: colour strip (green up,
 *   amber partly up, red down, grey not checked yet), name, what it is,
 *   checks up / total and the answer time or the problem. Problems first
 *   unless switched off in the menu.
 * - Tap a card for every check: what it tests, state, time, since when.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "app_framework.h"
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

static void fill_detail(void)
{
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
        int y = 66 + i * 62;
        lv_obj_t *row = lv_obj_create(U.dbody);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, AREA_W - 40, 54);
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
        lv_obj_set_pos(l, 42, 5);
        if (k->kind == MON_PING) snprintf(buf, sizeof(buf), "Ping (ICMP echo)");
        else if (k->kind == MON_TCP) snprintf(buf, sizeof(buf), "TCP port %u", k->port);
        else snprintf(buf, sizeof(buf), "%s", k->url);
        l = label(row, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
        one_line(l, 400);
        lv_obj_set_pos(l, 42, 31);

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
        lv_obj_align(l, LV_ALIGN_TOP_RIGHT, -14, 5);
        l = label(row, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, since);
        lv_obj_align(l, LV_ALIGN_BOTTOM_RIGHT, -14, -6);
    }
    monitor_unlock();
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
    U.dbody = lv_obj_create(U.detail);
    lv_obj_remove_style_all(U.dbody);
    lv_obj_set_size(U.dbody, AREA_W - 40, AREA_H - 66);
    lv_obj_set_pos(U.dbody, 20, 60);
    lv_obj_clear_flag(U.dbody, LV_OBJ_FLAG_CLICKABLE);
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
                        "The list is dashboard/monitors.json on the SD card (format in the "
                        "README; monitor/monitors.example.json in the project). Copy a new one "
                        "with: tools/sdcard.py put --force monitors.json monitors.json\n"
                        "It's re-read automatically when it changes. Checks run every minute "
                        "(\"interval\" in the file); a check is down after two failures in a row.");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, AREA_W - 40);
    lv_obj_set_pos(l, 20, 284);
}

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
    lv_obj_set_width(U.summary, 560);
    lv_obj_align(U.summary, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_align(button(parent, LV_SYMBOL_REFRESH "  Check", 130, 40, C_BUTTON, check_now, NULL),
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
    memset(&U, 0, sizeof(U));                    /* the launcher deletes the objects */
}

const app_t monitor_app = {
    .name      = "Servers",
    .symbol    = LV_SYMBOL_LIST,
    .color_hex = 0x2E7D32,
    .create    = monitor_create,
    .destroy   = monitor_destroy,
};
