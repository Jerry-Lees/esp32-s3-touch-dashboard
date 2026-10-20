/*
 * Timers: countdown timers, a stopwatch and alarms (tabs). The counting
 * and the alerts live in clock_tools.c, so everything keeps going while
 * this app is closed; this file is only the screens.
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "app_framework.h"
#include "clock_tools.h"
#include "launcher.h"
#include "net_time.h"
#include "settings.h"

#define TAB_H      56
#define C_ACCENT   0x42A5F5
#define C_BTN      0x2A333C
#define C_GREEN    0x1B8F4A
#define C_RED      0x8E2430
#define CARD_H     78

static const int PRESETS_MIN[] = { 1, 3, 5, 10, 15, 30 };

static struct {
    lv_obj_t   *parent, *tv;
    lv_timer_t *timer;
    uint32_t    ticks;
    /* timer tab */
    lv_obj_t   *r_h, *r_m, *r_s, *none;
    lv_obj_t   *card[CT_TIMERS], *c_time[CT_TIMERS], *c_sub[CT_TIMERS], *c_bar[CT_TIMERS],
               *c_pause[CT_TIMERS];
    /* stopwatch tab */
    lv_obj_t   *sw_time, *sw_go, *sw_lap, *sw_laps;
    int         sw_shown_laps;
    /* alarms tab */
    lv_obj_t   *al_list, *al_warn, *al_add;
    /* alarm editor */
    lv_obj_t   *ed, *ed_h, *ed_m, *ed_ampm, *ed_day[7];
    int         ed_index;          /* -1 = new */
    bool        ed_on;
} U;

/* ---------- small widgets ---------- */

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, int w, int h, uint32_t color,
                        lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static void set_btn_text(lv_obj_t *b, const char *text)
{
    lv_label_set_text(lv_obj_get_child(b, 0), text);
}

static lv_obj_t *mk_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* A roller of numbers lo..hi ("%02d" if pad). */
static lv_obj_t *mk_roller(lv_obj_t *parent, int lo, int hi, bool pad, int w)
{
    static char opts[3][24 * 3 + 60 * 3 + 8];
    static int which;
    char *o = opts[which++ % 3];
    size_t k = 0;
    for (int v = lo; v <= hi; v++) k += sprintf(o + k, pad ? "%02d%s" : "%d%s", v, v < hi ? "\n" : "");
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, o, LV_ROLLER_MODE_NORMAL);   /* copies the text */
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(r, lv_color_hex(APP_COLOR_SUBTEXT), 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x161B20), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(C_ACCENT), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
    return r;
}

static lv_obj_t *mk_tab(const char *name)
{
    lv_obj_t *t = lv_tabview_add_tab(U.tv, name);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    return t;
}

/* ======================= Timer tab ======================= */

static void timer_started(int i)
{
    if (i < 0) launcher_toast("All 4 timers are in use. Remove one first.", 4000);
}

static void start_clicked(lv_event_t *e)
{
    (void)e;
    int64_t ms = ((int64_t)lv_roller_get_selected(U.r_h) * 3600 +
                  lv_roller_get_selected(U.r_m) * 60 + lv_roller_get_selected(U.r_s)) * 1000;
    if (ms == 0) {
        launcher_toast("Set a time first", 2500);
        return;
    }
    timer_started(ct_timer_start(ms));
}

static void preset_clicked(lv_event_t *e)
{
    int min = (int)(intptr_t)lv_event_get_user_data(e);
    timer_started(ct_timer_start((int64_t)min * 60000));
}

static void pause_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ct_timer_info_t t = ct_timer_info(i);
    ct_timer_pause(i, t.running);
}

static void restart_clicked(lv_event_t *e) { ct_timer_restart((int)(intptr_t)lv_event_get_user_data(e)); }
static void remove_clicked(lv_event_t *e)  { ct_timer_remove((int)(intptr_t)lv_event_get_user_data(e)); }

static void build_timer_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab, 12, 10, 380, 340);
    lv_obj_set_pos(mk_label(p, APP_FONT_MEDIUM, APP_COLOR_TEXT, "New timer"), 16, 10);
    static const char *const units[] = { "hours", "min", "sec" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = mk_roller(p, 0, i == 0 ? 23 : 59, i > 0, 108);
        lv_obj_set_pos(r, 16 + i * 120, 38);
        lv_obj_t *u = mk_label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, units[i]);
        lv_obj_align_to(u, r, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
        if (i == 0) U.r_h = r; else if (i == 1) U.r_m = r; else U.r_s = r;
    }
    lv_roller_set_selected(U.r_m, 5, LV_ANIM_OFF);
    lv_obj_t *go = mk_btn(p, LV_SYMBOL_PLAY "  Start", 348, 50, C_GREEN, start_clicked, NULL);
    lv_obj_set_pos(go, 16, 172);
    for (int i = 0; i < 6; i++) {
        char t[16];
        snprintf(t, sizeof(t), "%d min", PRESETS_MIN[i]);
        lv_obj_t *b = mk_btn(p, t, 108, 46, C_BTN, preset_clicked, (void *)(intptr_t)PRESETS_MIN[i]);
        lv_obj_set_pos(b, 16 + (i % 3) * 120, 234 + (i / 3) * 54);
    }

    U.none = mk_label(tab, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT,
                      "No timers yet.\nPick a time, or tap a preset.\nUp to 4 can run at once,\n"
                      "and they keep going when\nyou leave this app.");
    lv_obj_set_pos(U.none, 430, 40);
    for (int i = 0; i < CT_TIMERS; i++) {
        lv_obj_t *c = mk_panel(tab, 404, 10 + i * (CARD_H + 9), 384, CARD_H);
        U.card[i] = c;
        U.c_time[i] = mk_label(c, APP_FONT_LARGE, APP_COLOR_TEXT, "");
        lv_obj_set_pos(U.c_time[i], 14, 8);
        U.c_sub[i] = mk_label(c, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
        lv_obj_set_pos(U.c_sub[i], 14, 44);
        U.c_bar[i] = lv_bar_create(c);
        lv_obj_set_size(U.c_bar[i], 180, 6);
        lv_obj_set_pos(U.c_bar[i], 14, 64);
        lv_bar_set_range(U.c_bar[i], 0, 1000);
        lv_obj_set_style_bg_color(U.c_bar[i], lv_color_hex(0x3A4652), 0);
        lv_obj_set_style_bg_color(U.c_bar[i], lv_color_hex(0xFFB74D), LV_PART_INDICATOR);
        void *ud = (void *)(intptr_t)i;
        U.c_pause[i] = mk_btn(c, LV_SYMBOL_PAUSE, 54, 50, C_BTN, pause_clicked, ud);
        lv_obj_align(U.c_pause[i], LV_ALIGN_RIGHT_MID, -134, 0);
        lv_obj_align(mk_btn(c, LV_SYMBOL_REFRESH, 54, 50, C_BTN, restart_clicked, ud),
                     LV_ALIGN_RIGHT_MID, -72, 0);
        lv_obj_align(mk_btn(c, LV_SYMBOL_CLOSE, 54, 50, C_RED, remove_clicked, ud),
                     LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

static void update_timers(void)
{
    bool any = false;
    for (int i = 0; i < CT_TIMERS; i++) {
        ct_timer_info_t t = ct_timer_info(i);
        if (!t.used) {
            lv_obj_add_flag(U.card[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        any = true;
        lv_obj_clear_flag(U.card[i], LV_OBJ_FLAG_HIDDEN);
        char a[24], b[24], sub[48];
        clock_tools_fmt(a, sizeof(a), t.left_ms + 999, false);    /* round up: 0:00 = done */
        clock_tools_fmt(b, sizeof(b), t.total_ms, false);
        lv_label_set_text(U.c_time[i], a);
        snprintf(sub, sizeof(sub), "%s timer%s", b, t.running ? "" : "  -  paused");
        lv_label_set_text(U.c_sub[i], sub);
        lv_bar_set_value(U.c_bar[i], (int32_t)(t.total_ms ? t.left_ms * 1000 / t.total_ms : 0), LV_ANIM_OFF);
        set_btn_text(U.c_pause[i], t.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (any) lv_obj_add_flag(U.none, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(U.none, LV_OBJ_FLAG_HIDDEN);
}

/* ======================= Stopwatch tab ======================= */

static void update_stopwatch(bool full);

static void sw_go_clicked(lv_event_t *e)
{
    (void)e;
    if (ct_sw_running()) ct_sw_stop();
    else ct_sw_start();
    update_stopwatch(true);
}

static void sw_lap_clicked(lv_event_t *e)
{
    (void)e;
    if (ct_sw_running()) {
        if (!ct_sw_lap()) launcher_toast("That's the most laps it can keep (99)", 3000);
    } else {
        ct_sw_reset();                       /* stopped: this button is Reset */
    }
    update_stopwatch(true);
}

static void build_stopwatch_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab, 12, 10, 430, 340);
    U.sw_time = mk_label(p, APP_FONT_HUGE, APP_COLOR_TEXT, "0:00.00");
    lv_obj_align(U.sw_time, LV_ALIGN_TOP_MID, 0, 80);
    U.sw_go = mk_btn(p, "", 190, 70, C_GREEN, sw_go_clicked, NULL);
    lv_obj_align(U.sw_go, LV_ALIGN_BOTTOM_LEFT, 20, -30);
    U.sw_lap = mk_btn(p, "", 190, 70, C_BTN, sw_lap_clicked, NULL);
    lv_obj_align(U.sw_lap, LV_ALIGN_BOTTOM_RIGHT, -20, -30);

    lv_obj_t *lp = mk_panel(tab, 454, 10, 334, 340);
    lv_obj_set_pos(mk_label(lp, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Laps"), 16, 10);
    U.sw_laps = lv_obj_create(lp);
    lv_obj_remove_style_all(U.sw_laps);
    lv_obj_set_size(U.sw_laps, 318, 290);
    lv_obj_set_pos(U.sw_laps, 8, 44);
    lv_obj_set_flex_flow(U.sw_laps, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.sw_laps, 4, 0);
    lv_obj_set_style_pad_hor(U.sw_laps, 8, 0);
    lv_obj_set_scroll_dir(U.sw_laps, LV_DIR_VER);
    U.sw_shown_laps = -1;
    update_stopwatch(true);
}

static void update_stopwatch(bool full)
{
    char t[24];
    clock_tools_fmt(t, sizeof(t), ct_sw_elapsed_ms(), true);
    lv_label_set_text(U.sw_time, t);
    if (!full) return;
    bool run = ct_sw_running();
    set_btn_text(U.sw_go, run ? LV_SYMBOL_PAUSE "  Stop" : LV_SYMBOL_PLAY "  Start");
    lv_obj_set_style_bg_color(U.sw_go, lv_color_hex(run ? C_RED : C_GREEN), 0);
    set_btn_text(U.sw_lap, run ? LV_SYMBOL_PLUS "  Lap" : LV_SYMBOL_REFRESH "  Reset");

    const int64_t *laps;
    int n = ct_sw_laps(&laps);
    if (n == U.sw_shown_laps) return;
    U.sw_shown_laps = n;
    lv_obj_clean(U.sw_laps);
    if (n == 0) {
        mk_label(U.sw_laps, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "Tap Lap while it runs.");
        return;
    }
    for (int i = n - 1; i >= 0; i--) {                    /* newest first */
        char split[24], total[24], line[80];
        clock_tools_fmt(split, sizeof(split), laps[i] - (i ? laps[i - 1] : 0), true);
        clock_tools_fmt(total, sizeof(total), laps[i], true);
        snprintf(line, sizeof(line), "Lap %-3d   %s   (%s)", i + 1, split, total);
        mk_label(U.sw_laps, APP_FONT_MEDIUM, i == n - 1 ? 0xFFB74D : APP_COLOR_TEXT, line);
    }
}

/* ======================= Alarms tab ======================= */

static void rebuild_alarms(void *arg);
static void editor_open(int index);

static void al_switch(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ct_alarm_t a = ct_alarm_get(i);
    a.on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ct_alarm_set(i, a);
}

static void al_edit(lv_event_t *e) { editor_open((int)(intptr_t)lv_event_get_user_data(e)); }
static void al_add(lv_event_t *e)  { (void)e; editor_open(-1); }

static void build_alarms_tab(lv_obj_t *tab)
{
    U.al_warn = mk_label(tab, LV_FONT_DEFAULT, 0xFFB74D,
                         "The clock isn't set yet (no Wi-Fi time and no saved time), so alarms "
                         "can't go off until it is.");
    lv_obj_set_width(U.al_warn, 560);
    lv_label_set_long_mode(U.al_warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(U.al_warn, 16, 16);
    U.al_add = mk_btn(tab, LV_SYMBOL_PLUS "  Add alarm", 180, 50, C_ACCENT, al_add, NULL);
    lv_obj_align(U.al_add, LV_ALIGN_TOP_RIGHT, -12, 8);
    U.al_list = lv_obj_create(tab);
    lv_obj_remove_style_all(U.al_list);
    lv_obj_set_size(U.al_list, 800, 292);
    lv_obj_set_pos(U.al_list, 0, 68);
    lv_obj_set_flex_flow(U.al_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.al_list, 8, 0);
    lv_obj_set_style_pad_hor(U.al_list, 12, 0);
    lv_obj_set_style_pad_bottom(U.al_list, 8, 0);
    lv_obj_set_scroll_dir(U.al_list, LV_DIR_VER);
    rebuild_alarms(NULL);
}

static void rebuild_alarms(void *arg)
{
    (void)arg;
    if (!U.al_list) return;
    lv_obj_clean(U.al_list);
    int n = ct_alarm_count();
    if (n >= CT_ALARMS) lv_obj_add_state(U.al_add, LV_STATE_DISABLED);
    else lv_obj_clear_state(U.al_add, LV_STATE_DISABLED);
    if (n == 0) {
        mk_label(U.al_list, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT,
                 "No alarms. Tap \"Add alarm\" (up to 6).\n"
                 "When one goes off, the screen wakes and flashes until you dismiss it.");
        return;
    }
    for (int i = 0; i < n; i++) {
        ct_alarm_t a = ct_alarm_get(i);
        lv_obj_t *row = mk_panel(U.al_list, 0, 0, 776, 64);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2A3440), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, al_edit, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[24], d[40];
        ct_alarm_time_text(a.hour, a.min, t, sizeof(t));
        ct_alarm_days_text(a.days, d, sizeof(d));
        lv_obj_t *tl = mk_label(row, APP_FONT_LARGE, a.on ? APP_COLOR_TEXT : APP_COLOR_SUBTEXT, t);
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 18, 0);
        lv_obj_t *dl = mk_label(row, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, d);
        lv_obj_align(dl, LV_ALIGN_LEFT_MID, 220, 0);
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 74, 40);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
        if (a.on) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, al_switch, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
    }
}

/* ---------- alarm editor (a page over the tabs) ---------- */

static void editor_close(void)
{
    if (U.ed) lv_obj_del_async(U.ed);          /* called from its own buttons */
    U.ed = NULL;
}

static void ed_save(lv_event_t *e)
{
    (void)e;
    ct_alarm_t a = { .on = true };
    int h = lv_roller_get_selected(U.ed_h);
    if (U.ed_ampm) {                           /* 12-hour: roller is 1..12 */
        h = (h + 1) % 12 + (lv_roller_get_selected(U.ed_ampm) ? 12 : 0);
    }
    a.hour = h;
    a.min = lv_roller_get_selected(U.ed_m);
    for (int d = 0; d < 7; d++) {
        if (lv_obj_has_state(U.ed_day[d], LV_STATE_CHECKED)) a.days |= 1 << d;
    }
    if (U.ed_index < 0) ct_alarm_add(a);
    else ct_alarm_set(U.ed_index, a);
    editor_close();
    lv_async_call(rebuild_alarms, NULL);
    char when[24], msg[64];
    ct_alarm_time_text(a.hour, a.min, when, sizeof(when));
    snprintf(msg, sizeof(msg), "Alarm set for %s", when);
    launcher_toast(msg, 2500);
}

static void ed_cancel(lv_event_t *e) { (void)e; editor_close(); }

static void ed_delete(lv_event_t *e)
{
    (void)e;
    ct_alarm_remove(U.ed_index);
    editor_close();
    lv_async_call(rebuild_alarms, NULL);
}

static void ed_days(lv_event_t *e)
{
    uint8_t mask = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    for (int d = 0; d < 7; d++) {
        if (mask & (1 << d)) lv_obj_add_state(U.ed_day[d], LV_STATE_CHECKED);
        else lv_obj_clear_state(U.ed_day[d], LV_STATE_CHECKED);
    }
}

static void editor_open(int index)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    ct_alarm_t a = index >= 0 ? ct_alarm_get(index) : (ct_alarm_t){ .hour = 7, .min = 0, .days = 0x3E };
    U.ed_index = index;
    U.ed = lv_obj_create(U.parent);
    lv_obj_remove_style_all(U.ed);
    lv_obj_set_size(U.ed, 800, 416);
    lv_obj_set_style_bg_color(U.ed, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(U.ed, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.ed, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(U.ed, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_pos(mk_label(U.ed, APP_FONT_LARGE, APP_COLOR_TEXT, index < 0 ? "New alarm" : "Edit alarm"),
                   20, 18);
    lv_obj_align(mk_btn(U.ed, LV_SYMBOL_OK "  Save", 140, 50, C_GREEN, ed_save, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_align(mk_btn(U.ed, "Cancel", 120, 50, C_BTN, ed_cancel, NULL), LV_ALIGN_TOP_RIGHT, -164, 10);
    if (index >= 0) {
        lv_obj_align(mk_btn(U.ed, LV_SYMBOL_TRASH "  Delete", 140, 50, C_RED, ed_delete, NULL),
                     LV_ALIGN_TOP_RIGHT, -296, 10);
    }

    bool h24 = settings_get_24h();
    U.ed_h = mk_roller(U.ed, h24 ? 0 : 1, h24 ? 23 : 12, h24, 110);
    lv_obj_set_pos(U.ed_h, 40, 84);
    lv_obj_t *colon = mk_label(U.ed, APP_FONT_HUGE, APP_COLOR_TEXT, ":");
    lv_obj_set_pos(colon, 160, 112);
    U.ed_m = mk_roller(U.ed, 0, 59, true, 110);
    lv_obj_set_pos(U.ed_m, 190, 84);
    U.ed_ampm = NULL;
    if (h24) {
        lv_roller_set_selected(U.ed_h, a.hour, LV_ANIM_OFF);
    } else {
        lv_roller_set_selected(U.ed_h, (a.hour + 11) % 12, LV_ANIM_OFF);   /* 1..12 */
        U.ed_ampm = lv_roller_create(U.ed);
        lv_roller_set_options(U.ed_ampm, "AM\nPM", LV_ROLLER_MODE_NORMAL);
        lv_roller_set_visible_row_count(U.ed_ampm, 2);
        lv_obj_set_width(U.ed_ampm, 100);
        lv_obj_set_style_text_font(U.ed_ampm, APP_FONT_LARGE, 0);
        lv_obj_set_style_text_color(U.ed_ampm, lv_color_hex(APP_COLOR_SUBTEXT), 0);
        lv_obj_set_style_bg_color(U.ed_ampm, lv_color_hex(0x161B20), 0);
        lv_obj_set_style_border_width(U.ed_ampm, 0, 0);
        lv_obj_set_style_bg_color(U.ed_ampm, lv_color_hex(C_ACCENT), LV_PART_SELECTED);
        lv_obj_set_style_text_color(U.ed_ampm, lv_color_white(), LV_PART_SELECTED);
        lv_obj_set_pos(U.ed_ampm, 320, 100);
        lv_roller_set_selected(U.ed_ampm, a.hour >= 12, LV_ANIM_OFF);
    }
    lv_roller_set_selected(U.ed_m, a.min, LV_ANIM_OFF);

    lv_obj_t *rl = mk_label(U.ed, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Repeat on");
    lv_obj_set_pos(rl, 460, 84);
    lv_obj_t *hint = mk_label(U.ed, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                              "No days picked = once\n(it turns itself off after ringing).");
    lv_obj_set_pos(hint, 460, 112);
    for (int i = 0; i < 3; i++) {
        static const char *const qn[3] = { "Weekdays", "Every day", "Once" };
        static const uint8_t qm[3] = { 0x3E, 0x7F, 0x00 };
        lv_obj_t *b = mk_btn(U.ed, qn[i], 104, 44, C_BTN, ed_days, (void *)(intptr_t)qm[i]);
        lv_obj_set_style_pad_hor(b, 2, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), LV_FONT_DEFAULT, 0);
        lv_obj_set_pos(b, 460 + i * 110, 160);
    }
    for (int d = 0; d < 7; d++) {
        lv_obj_t *b = mk_btn(U.ed, names[d], 100, 56, C_BTN, NULL, NULL);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_pos(b, 40 + d * 104, 300);
        if (a.days & (1 << d)) lv_obj_add_state(b, LV_STATE_CHECKED);
        U.ed_day[d] = b;
    }
}

/* ======================= app ======================= */

static void tick(lv_timer_t *t)
{
    (void)t;
    U.ticks++;
    uint16_t tab = lv_tabview_get_tab_act(U.tv);
    if (tab == 1) update_stopwatch(U.ticks % 5 == 0);
    if (U.ticks % 5 == 0) {
        if (tab == 0) update_timers();
        if (tab == 2) {
            if (net_time_is_synced()) lv_obj_add_flag(U.al_warn, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_clear_flag(U.al_warn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* A one-time alarm turns itself off when it rings: show that. */
    static int seen_on = -1;
    int on = 0;
    for (int i = 0; i < ct_alarm_count(); i++) on += ct_alarm_get(i).on;
    if (seen_on >= 0 && on != seen_on && !U.ed) rebuild_alarms(NULL);
    seen_on = on;
}

static void timers_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.parent = parent;
    U.tv = lv_tabview_create(parent, LV_DIR_TOP, TAB_H);
    lv_obj_set_size(U.tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(U.tv, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(U.tv, LV_OPA_COVER, 0);
    lv_obj_t *btns = lv_tabview_get_tab_btns(U.tv);
    lv_obj_set_style_bg_color(btns, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_text_font(btns, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(btns, lv_color_hex(APP_COLOR_SUBTEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_color(btns, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btns, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_t *content = lv_tabview_get_content(U.tv);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);       /* tabs change by tapping only */
    lv_obj_set_style_bg_color(content, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    build_timer_tab(mk_tab(LV_SYMBOL_LOOP " Timer"));
    build_stopwatch_tab(mk_tab(LV_SYMBOL_PLAY " Stopwatch"));
    build_alarms_tab(mk_tab(LV_SYMBOL_BELL " Alarms"));
    /* Open on the tab that's in use. */
    if (ct_sw_running()) lv_tabview_set_act(U.tv, 1, LV_ANIM_OFF);
    update_timers();
    U.timer = lv_timer_create(tick, 40, NULL);
}

static void timers_destroy(void)
{
    if (U.timer) lv_timer_del(U.timer);
    memset(&U, 0, sizeof(U));                 /* the launcher deletes the objects */
}

const app_t timers_app = {
    .name      = "Timers",
    .symbol    = LV_SYMBOL_BELL,
    .color_hex = 0xEF6C00,
    .create    = timers_create,
    .destroy   = timers_destroy,
};
