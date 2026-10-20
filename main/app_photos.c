/*
 * Photos: slideshow of dashboard/pictures on the SD card (engine in
 * photo_frame.c). Tap the picture to show or hide the controls; "Full
 * screen" covers the top bar too (tap to come back). The screen timeout
 * is skipped while this app is open, so it works as a photo frame.
 * The trash button deletes the picture from the card (after asking).
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "app_framework.h"
#include "game_common.h"
#include "launcher.h"
#include "photo_frame.h"

#define BAR_H      68
#define HIDE_MS    5000
#define C_BAR      0x101418

static struct {
    lv_obj_t   *parent, *view, *bar, *name, *full;
    lv_obj_t   *b_play, *b_int, *b_shuf, *b_fill;
    lv_timer_t *timer;
    uint32_t    bar_shown_ms;
    bool        bar_visible;
    bool        asking, was_paused;      /* delete dialog open; pause state before it */
} U;

static lv_obj_t *mk_btn(const char *text, int w, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(U.bar);
    lv_obj_set_size(b, w, 50);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A333C), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
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

static void refresh_labels(void)
{
    char buf[32];
    set_btn_text(U.b_play, photo_frame_paused() ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    int s = photo_frame_interval_s();
    if (s < 60) snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP " %d s", s);
    else snprintf(buf, sizeof(buf), LV_SYMBOL_LOOP " %d min", s / 60);
    set_btn_text(U.b_int, buf);
    set_btn_text(U.b_shuf, photo_frame_shuffle() ? LV_SYMBOL_SHUFFLE " On" : LV_SYMBOL_SHUFFLE " Off");
    set_btn_text(U.b_fill, photo_frame_fill() ? "Fill" : "Fit");
}

static void show_bar(bool on)
{
    U.bar_visible = on;
    U.bar_shown_ms = lv_tick_get();
    if (on) {
        lv_obj_clear_flag(U.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(U.name, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(U.name);
        lv_obj_move_foreground(U.bar);
    } else {
        lv_obj_add_flag(U.bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.name, LV_OBJ_FLAG_HIDDEN);
    }
}

static void bar_used(void)
{
    U.bar_shown_ms = lv_tick_get();     /* keep it up while it's being used */
    refresh_labels();
}

static void prev_clicked(lv_event_t *e)  { (void)e; photo_frame_step(-1); bar_used(); }
static void next_clicked(lv_event_t *e)  { (void)e; photo_frame_step(+1); bar_used(); }

static void play_clicked(lv_event_t *e)
{
    (void)e;
    photo_frame_set_paused(!photo_frame_paused());
    bar_used();
}

static void delete_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    U.asking = false;
    if (yes && !photo_frame_delete_current()) {
        launcher_toast("Couldn't delete the picture", 4000);
    }
    photo_frame_set_paused(U.was_paused);
    bar_used();
}

static void delete_clicked(lv_event_t *e)
{
    (void)e;
    const char *name = photo_frame_name();
    if (!name[0]) return;                /* nothing on screen (yet) */
    /* Paused while asking, so the picture can't change under the question. */
    U.was_paused = photo_frame_paused();
    photo_frame_set_paused(true);
    U.asking = true;
    char title[200];
    snprintf(title, sizeof(title), "Delete this picture from the SD card?\n\n%s", name);
    lv_obj_t *row = game_dialog_open(U.parent, title);
    game_dialog_button(row, LV_SYMBOL_TRASH "  Delete", delete_answer, (void *)1);
    game_dialog_button(row, "Cancel", delete_answer, NULL);
}

static void int_clicked(lv_event_t *e)  { (void)e; photo_frame_cycle_interval(); bar_used(); }
static void shuf_clicked(lv_event_t *e) { (void)e; photo_frame_set_shuffle(!photo_frame_shuffle()); bar_used(); }
static void fill_clicked(lv_event_t *e) { (void)e; photo_frame_set_fill(!photo_frame_fill()); bar_used(); }

static void open_in_app(void)
{
    U.view = photo_frame_open(U.parent, 800, 416);
    lv_obj_move_background(U.view);
}

static void full_tapped(lv_event_t *e)
{
    (void)e;
    photo_frame_close();
    lv_obj_del_async(U.full);           /* we're inside its event */
    U.full = NULL;
    open_in_app();
    show_bar(true);
}

static void full_clicked(lv_event_t *e)
{
    (void)e;
    bool paused = photo_frame_paused();
    photo_frame_close();
    lv_obj_del(U.view);
    U.view = NULL;
    show_bar(false);

    U.full = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(U.full);
    lv_obj_set_size(U.full, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(U.full, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.full, full_tapped, LV_EVENT_CLICKED, NULL);
    photo_frame_open(U.full, 800, 480);
    photo_frame_set_paused(paused);
}

static void area_clicked(lv_event_t *e)
{
    if (lv_event_get_target(e) != U.parent) return;   /* a button, not the picture */
    show_bar(!U.bar_visible);
    if (U.bar_visible) refresh_labels();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (U.full) return;
    int n, total;
    photo_frame_position(&n, &total);
    const char *name = photo_frame_name();
    if (name[0]) lv_label_set_text_fmt(U.name, "%s   (%d of %d)", name, n, total);
    else lv_label_set_text(U.name, "");
    if (U.bar_visible && !U.asking && photo_frame_state() == PF_SHOWING &&
        lv_tick_elaps(U.bar_shown_ms) > HIDE_MS) {
        show_bar(false);
    }
}

static void photos_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.parent = parent;
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, area_clicked, LV_EVENT_CLICKED, NULL);

    open_in_app();

    U.name = lv_label_create(parent);
    lv_obj_set_style_text_font(U.name, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(U.name, lv_color_white(), 0);
    lv_obj_set_style_bg_color(U.name, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.name, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(U.name, 6, 0);
    lv_obj_set_style_radius(U.name, 6, 0);
    lv_label_set_text(U.name, "");
    lv_obj_align(U.name, LV_ALIGN_BOTTOM_LEFT, 10, -BAR_H - 8);

    U.bar = lv_obj_create(parent);
    lv_obj_remove_style_all(U.bar);
    lv_obj_set_size(U.bar, LV_PCT(100), BAR_H);
    lv_obj_align(U.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(U.bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(U.bar, LV_OPA_80, 0);
    lv_obj_set_flex_flow(U.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(U.bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(U.bar, LV_OBJ_FLAG_CLICKABLE);    /* taps on its gaps don't reach the picture */

    mk_btn(LV_SYMBOL_PREV, 64, prev_clicked);
    U.b_play = mk_btn(LV_SYMBOL_PAUSE, 64, play_clicked);
    mk_btn(LV_SYMBOL_NEXT, 64, next_clicked);
    U.b_int  = mk_btn("", 112, int_clicked);
    U.b_shuf = mk_btn("", 112, shuf_clicked);
    U.b_fill = mk_btn("", 80, fill_clicked);
    mk_btn(LV_SYMBOL_UP "  Full", 112, full_clicked);
    lv_obj_t *del = mk_btn(LV_SYMBOL_TRASH, 64, delete_clicked);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x8E2430), 0);
    refresh_labels();
    show_bar(true);

    U.timer = lv_timer_create(tick, 250, NULL);
}

static void photos_destroy(void)
{
    if (U.timer) lv_timer_del(U.timer);
    photo_frame_close();
    if (U.full) lv_obj_del(U.full);     /* on the top layer: not deleted with the app */
    memset(&U, 0, sizeof(U));
}

const app_t photos_app = {
    .name      = "Photos",
    .symbol    = LV_SYMBOL_IMAGE,
    .color_hex = 0xAD1457,
    .create    = photos_create,
    .destroy   = photos_destroy,
};
