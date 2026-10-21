/*
 * Info ticker. Each item (one weather city, one stock) is its own label;
 * the labels march left together and a new one is appended on the right
 * whenever there's room. Labels that leave on the left are deleted. Using
 * one label per item keeps each label well under LVGL's 8191 px coordinate
 * limit, and each item's text is built when it enters, so it's always the
 * latest data.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "app_framework.h"
#include "settings.h"
#include "ticker.h"

#define SPEED      110         /* px per second */
#define GAP        44          /* between an item and a separator */
#define MAX_LABELS 16
#define MAX_ITEMS  (8 + 20 + 1)    /* weather cities + stocks + servers */

#define C_BAR      0x0B0F13
#define C_EDGE     0x2A333C

static struct {
    lv_obj_t   *bar;
    lv_obj_t   *lbl[MAX_LABELS];   /* left to right */
    float       x[MAX_LABELS];
    lv_coord_t  w[MAX_LABELS];
    int         n;
    int         next;              /* index of the next item to show */
    bool        sep_next;          /* next label is a separator */
    bool        running;
    int64_t     last_us;
    lv_timer_t *timer;
    char      (*items)[TICKER_ITEM_LEN];
} T;

static int collect_items(void)
{
    if (!T.items) {
        T.items = heap_caps_malloc(sizeof(*T.items) * MAX_ITEMS, MALLOC_CAP_SPIRAM);
        if (!T.items) return 0;
    }
    int n = 0;
#if CONFIG_DASH_APP_WEATHER
    if (settings_get_ticker_weather()) {
        n += weather_ticker_items(T.items + n, MAX_ITEMS - n);
    }
#endif
#if CONFIG_DASH_APP_STONKS
    if (settings_get_ticker_stocks()) {
        n += stonks_ticker_items(T.items + n, MAX_ITEMS - n);
    }
#endif
#if CONFIG_DASH_APP_SERVERS
    n += monitor_ticker_items(T.items + n, MAX_ITEMS - n);
#endif
    return n;
}

static void append_label(lv_coord_t x)
{
    if (T.n >= MAX_LABELS) return;
    const char *text;
    if (T.sep_next) {
        text = "#5F6B77 " LV_SYMBOL_BULLET "#";
    } else {
        int n = collect_items();
        if (n == 0) {
            text = (settings_get_ticker_weather() || settings_get_ticker_stocks())
                   ? "#9AA5B1 Waiting for data...#"
                   : "#9AA5B1 Nothing to show: pick ticker items in Settings#";
        } else {
            if (T.next >= n) T.next = 0;
            text = T.items[T.next++];
        }
    }
    T.sep_next = !T.sep_next;

    lv_obj_t *l = lv_label_create(T.bar);
    lv_obj_set_style_text_font(l, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, text);
    lv_obj_update_layout(l);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);

    T.lbl[T.n] = l;
    T.x[T.n] = x;
    T.w[T.n] = lv_obj_get_width(l);
    T.n++;
}

static void step(lv_timer_t *t)
{
    (void)t;
    int64_t now = esp_timer_get_time();
    float dt = (now - T.last_us) / 1e6f;
    T.last_us = now;
    if (!T.running) return;
    if (dt > 0.1f) dt = 0.1f;

    for (int i = 0; i < T.n; i++) {
        T.x[i] -= SPEED * dt;
        lv_obj_set_x(T.lbl[i], (lv_coord_t)T.x[i]);
    }
    while (T.n > 0 && T.x[0] + T.w[0] < 0) {     /* gone off the left */
        lv_obj_del(T.lbl[0]);
        memmove(&T.lbl[0], &T.lbl[1], sizeof(T.lbl[0]) * (T.n - 1));
        memmove(&T.x[0], &T.x[1], sizeof(T.x[0]) * (T.n - 1));
        memmove(&T.w[0], &T.w[1], sizeof(T.w[0]) * (T.n - 1));
        T.n--;
    }
    lv_coord_t bar_w = lv_obj_get_width(T.bar);
    for (;;) {
        lv_coord_t right = T.n ? (lv_coord_t)(T.x[T.n - 1] + T.w[T.n - 1] + GAP)
                               : bar_w;
        if (right > bar_w || T.n >= MAX_LABELS) break;
        append_label(right);
    }
}

static void bar_deleted(lv_event_t *e)
{
    (void)e;
    if (T.timer) lv_timer_del(T.timer);
    char (*items)[TICKER_ITEM_LEN] = T.items;   /* keep the buffer for next time */
    memset(&T, 0, sizeof(T));
    T.items = items;
}

lv_obj_t *ticker_create(lv_obj_t *parent)
{
    if (T.bar) lv_obj_del(T.bar);

    T.bar = lv_obj_create(parent);
    lv_obj_remove_style_all(T.bar);
    lv_obj_set_size(T.bar, LV_PCT(100), TICKER_H);
    lv_obj_align(T.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(T.bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(T.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(T.bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(T.bar, lv_color_hex(C_EDGE), 0);
    lv_obj_set_style_border_width(T.bar, 2, 0);
    lv_obj_clear_flag(T.bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(T.bar, bar_deleted, LV_EVENT_DELETE, NULL);

    T.running = true;
    T.last_us = esp_timer_get_time();
    T.timer = lv_timer_create(step, 20, NULL);
    return T.bar;
}

void ticker_set_running(bool on)
{
    if (on && !T.running) {
        T.last_us = esp_timer_get_time();   /* don't jump after a pause */
    }
    T.running = on;
}

void ticker_service(void)
{
    if (!settings_get_ticker()) return;
#if CONFIG_DASH_APP_WEATHER
    if (settings_get_ticker_weather()) weather_background_tick();
#endif
#if CONFIG_DASH_APP_STONKS
    if (settings_get_ticker_stocks()) stonks_background_tick();
#endif
}
