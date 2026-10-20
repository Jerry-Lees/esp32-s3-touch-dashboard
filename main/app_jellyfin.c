/*
 * Jellyfin: what's playing on the user's Jellyfin server, with cover art,
 * a progress bar (drag to seek) and previous / play-pause / next / stop.
 * When several devices are playing, arrows switch between them.
 *
 * The server is set up behind the gear button (address + API key, kept by
 * jellyfin.c in its own NVS namespace, never in settings backups). All the
 * network work happens in jellyfin.c's task; this file only reads
 * jf_snapshot() from a 250 ms lv_timer and moves the progress bar in between.
 */
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "lvgl.h"
#include "app_framework.h"
#include "clock_tools.h"
#include "jellyfin.h"
#include "launcher.h"

#define C_ACCENT  0xAA5CC3          /* Jellyfin purple */
#define C_BUTTON  0x2A333C
#define ART_X     24
#define ART_Y     24
#define COL_X     352               /* right-hand column */
#define COL_W     424

static struct {
    lv_obj_t   *root, *main, *msg_view, *msg_icon, *msg_text, *msg_btn;
    lv_obj_t   *art_box, *art_img, *art_icon;
    lv_obj_t   *who, *title, *line2, *line3, *badge, *no_ctrl;
    lv_obj_t   *sw_prev, *sw_next, *sw_label;
    lv_obj_t   *btn_prev, *btn_play, *btn_next, *btn_stop, *play_icon;
    lv_obj_t   *slider, *t_pos, *t_len;
    lv_timer_t *timer;
    lv_img_dsc_t dsc[2];
    int         dsc_i;
    char        art_shown[80];      /* art key on screen ("" = icon) */
    jf_snapshot_t snap;
    uint32_t    shown_version;
    char        sel_id[40];         /* selected session */
    char        sel_item[40];
    bool        seeking;            /* finger on the progress bar */
    int         want_paused;        /* after a tap: 0/1 until the server agrees, else -1 */
    int64_t     want_until_us;
    /* setup page */
    lv_obj_t   *setup, *ta_url, *ta_key, *kb;
} U;

/* ---------- helpers ---------- */

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *p, const char *text, int w, int h, uint32_t color,
                        lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = mk_label(b, APP_FONT_MEDIUM, APP_COLOR_TEXT, text);
    lv_obj_center(l);
    return b;
}

static void set_enabled(lv_obj_t *o, bool on)
{
    if (on) lv_obj_clear_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

static const jf_session_t *selected(void)
{
    for (int i = 0; i < U.snap.n; i++) {
        if (!strcmp(U.snap.s[i].id, U.sel_id)) return &U.snap.s[i];
    }
    return NULL;
}

static int selected_index(void)
{
    const jf_session_t *s = selected();
    return s ? (int)(s - U.snap.s) : -1;
}

static bool is_paused(const jf_session_t *s)
{
    return U.want_paused >= 0 ? U.want_paused : s->paused;
}

static int64_t position_ms(const jf_session_t *s)
{
    int64_t pos = s->pos_ms;
    if (!is_paused(s)) pos += (esp_timer_get_time() - U.snap.at_us) / 1000;
    if (s->runtime_ms > 0 && pos > s->runtime_ms) pos = s->runtime_ms;
    return pos;
}

/* ---------- drawing ---------- */

static void show_art(const char *key, const uint16_t *px, int w, int h)
{
    snprintf(U.art_shown, sizeof(U.art_shown), "%s", key);
    if (!px || w <= 0 || h <= 0) {
        lv_obj_add_flag(U.art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(U.art_icon, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* Alternate between two descriptors so LVGL's image cache can't show
     * the old picture. */
    U.dsc_i ^= 1;
    lv_img_dsc_t *d = &U.dsc[U.dsc_i];
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w = w;
    d->header.h = h;
    d->data_size = (uint32_t)w * h * 2;
    d->data = (const uint8_t *)px;
    lv_img_cache_invalidate_src(d);
    lv_img_set_src(U.art_img, d);
    lv_obj_center(U.art_img);
    lv_obj_clear_flag(U.art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.art_icon, LV_OBJ_FLAG_HIDDEN);
}

static void show_message(const char *icon, const char *text, bool setup_btn)
{
    lv_obj_add_flag(U.main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(U.msg_view, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(U.msg_icon, icon);
    lv_label_set_text(U.msg_text, text);
    if (setup_btn) lv_obj_clear_flag(U.msg_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.msg_btn, LV_OBJ_FLAG_HIDDEN);
}

static void update_progress(const jf_session_t *s)
{
    char a[16], b[16];
    int64_t pos = position_ms(s);
    clock_tools_fmt(a, sizeof(a), pos, false);
    lv_label_set_text(U.t_pos, a);
    if (s->runtime_ms > 0) {
        clock_tools_fmt(b, sizeof(b), s->runtime_ms, false);
        lv_label_set_text(U.t_len, b);
        if (!U.seeking) lv_slider_set_value(U.slider, (int32_t)(pos * 1000 / s->runtime_ms), LV_ANIM_OFF);
    } else {
        lv_label_set_text(U.t_len, "");
        if (!U.seeking) lv_slider_set_value(U.slider, 0, LV_ANIM_OFF);
    }
}

/* Redraws everything from U.snap. */
static void update_view(void)
{
    switch (U.snap.state) {
    case JF_NOT_SET:
        show_message(LV_SYMBOL_SETTINGS, "Jellyfin isn't set up yet.\nTap the button to enter "
                     "your server's address and an API key.", true);
        return;
    case JF_NO_WIFI:
        show_message(LV_SYMBOL_WIFI, "Waiting for Wi-Fi...", false);
        return;
    case JF_CONNECTING:
        show_message(LV_SYMBOL_REFRESH, "Asking the server what's playing...", false);
        return;
    case JF_ERROR: {
        char t[160];
        snprintf(t, sizeof(t), "%s.%s", U.snap.msg, strstr(U.snap.msg, "key") ?
                 "\nCheck the API key (gear button)." : "\nIt will keep trying.");
        show_message(LV_SYMBOL_WARNING, t, true);
        return;
    }
    case JF_OK:
        break;
    }
    if (U.snap.n == 0) {
        show_message(LV_SYMBOL_PLAY, "Nothing is playing.\nStart something in any Jellyfin "
                     "app and it shows up here.", false);
        jf_art_want("");
        return;
    }
    if (!selected()) snprintf(U.sel_id, sizeof(U.sel_id), "%s", U.snap.s[0].id);
    const jf_session_t *s = selected();

    lv_obj_clear_flag(U.main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.msg_view, LV_OBJ_FLAG_HIDDEN);

    /* Forget the tapped play/pause state once the server agrees (or after a
     * few seconds, or on a new track). */
    if (strcmp(U.sel_item, s->item)) {
        snprintf(U.sel_item, sizeof(U.sel_item), "%s", s->item);
        U.want_paused = -1;
    }
    if (U.want_paused >= 0 && (s->paused == U.want_paused ||
                               esp_timer_get_time() > U.want_until_us)) {
        U.want_paused = -1;
    }

    lv_label_set_text(U.who, s->who);
    lv_label_set_text(U.title, s->title);
    lv_label_set_text(U.line2, s->line2);
    lv_label_set_text(U.line3, s->line3);
    lv_label_set_text(U.art_icon, s->audio ? LV_SYMBOL_AUDIO : LV_SYMBOL_VIDEO);

    lv_label_set_text(U.badge, is_paused(s) ? LV_SYMBOL_PAUSE "  Paused" : "");
    lv_label_set_text(U.play_icon, is_paused(s) ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    /* Players that don't take commands (e.g. the Roku app): no buttons. */
    lv_obj_t *ctrl[] = { U.btn_prev, U.btn_play, U.btn_next, U.btn_stop };
    for (int i = 0; i < 4; i++) {
        if (s->can_control) lv_obj_clear_flag(ctrl[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(ctrl[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s->can_control) {
        lv_obj_add_flag(U.no_ctrl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text_fmt(U.no_ctrl, "%s doesn't accept remote control, so it can only "
                              "be watched from here.", s->client[0] ? s->client : "This player");
        lv_obj_clear_flag(U.no_ctrl, LV_OBJ_FLAG_HIDDEN);
    }
    set_enabled(U.slider, s->can_control && s->can_seek && s->runtime_ms > 0);

    if (U.snap.n > 1) {
        lv_label_set_text_fmt(U.sw_label, "%d of %d", selected_index() + 1, U.snap.n);
        lv_obj_clear_flag(U.sw_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(U.sw_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(U.sw_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(U.sw_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.sw_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.sw_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (strcmp(U.art_shown, s->art)) {
        if (!s->art[0]) show_art("", NULL, 0, 0);
        jf_art_want(s->art);
    }
    update_progress(s);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    jf_snapshot(&U.snap);
    char key[80];
    const uint16_t *px;
    int w, h;
    if (jf_art_take(key, sizeof(key), &px, &w, &h)) show_art(key, px, w, h);

    if (U.snap.version != U.shown_version) {
        U.shown_version = U.snap.version;
        update_view();
    } else if (U.snap.state == JF_OK && selected()) {
        update_progress(selected());
    }
}

/* ---------- controls ---------- */

static void command(jf_cmd_t cmd)
{
    const jf_session_t *s = selected();
    if (!s || !s->can_control) return;
    jf_command(s->id, cmd, 0);
    if (cmd == JF_CMD_PLAYPAUSE) {
        /* Show the change at once; the next answers confirm it. */
        U.want_paused = !is_paused(s);
        U.want_until_us = esp_timer_get_time() + 4000 * 1000;
        update_view();
    }
}

static void prev_clicked(lv_event_t *e) { (void)e; command(JF_CMD_PREV); }
static void play_clicked(lv_event_t *e) { (void)e; command(JF_CMD_PLAYPAUSE); }
static void next_clicked(lv_event_t *e) { (void)e; command(JF_CMD_NEXT); }
static void stop_clicked(lv_event_t *e) { (void)e; command(JF_CMD_STOP); }

static void switch_session(int dir)
{
    int i = selected_index();
    if (U.snap.n < 2 || i < 0) return;
    i = (i + dir + U.snap.n) % U.snap.n;
    snprintf(U.sel_id, sizeof(U.sel_id), "%s", U.snap.s[i].id);
    update_view();
}

static void sw_prev_clicked(lv_event_t *e) { (void)e; switch_session(-1); }
static void sw_next_clicked(lv_event_t *e) { (void)e; switch_session(1); }

static void slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const jf_session_t *s = selected();
    if (code == LV_EVENT_PRESSED) {
        U.seeking = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && U.seeking && s && s->runtime_ms > 0) {
        char a[16];
        clock_tools_fmt(a, sizeof(a), lv_slider_get_value(U.slider) * s->runtime_ms / 1000, false);
        lv_label_set_text(U.t_pos, a);
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && U.seeking) {
        U.seeking = false;
        if (s && s->can_control && s->runtime_ms > 0) {
            int64_t ms = lv_slider_get_value(U.slider) * s->runtime_ms / 1000;
            jf_command(s->id, JF_CMD_SEEK, ms);
            U.snap.s[selected_index()].pos_ms = ms;
            U.snap.at_us = esp_timer_get_time();
        }
    }
}

/* ---------- setup page ---------- */

static void setup_close(void)
{
    if (!U.setup) return;
    lv_textarea_set_text(U.ta_key, "");
    lv_obj_del_async(U.setup);                  /* we may be inside its event */
    U.setup = U.ta_url = U.ta_key = U.kb = NULL;
}

static void setup_save(void)
{
    char why[96];
    if (!jf_save(lv_textarea_get_text(U.ta_url), lv_textarea_get_text(U.ta_key),
                 why, sizeof(why))) {
        launcher_toast(why, 4000);
        return;
    }
    if (!jf_configured()) {
        launcher_toast("Enter the API key too", 4000);
        return;
    }
    setup_close();
    launcher_toast("Jellyfin server saved", 2500);
}

static void setup_save_clicked(lv_event_t *e) { (void)e; setup_save(); }
static void setup_cancel_clicked(lv_event_t *e) { (void)e; setup_close(); }

static void forget_answer(lv_event_t *e)
{
    (void)e;
    jf_forget();
    setup_close();
    launcher_toast("Jellyfin server forgotten", 2500);
}

static void setup_forget_clicked(lv_event_t *e)
{
    (void)e;
    /* Second tap within the button's "Sure?" state forgets. */
    lv_obj_t *b = lv_event_get_target(e);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (strcmp(lv_label_get_text(l), "Sure?")) {
        lv_label_set_text(l, "Sure?");
        return;
    }
    forget_answer(e);
}

static void kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) setup_save();
    else setup_close();
}

static void ta_focused(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_clear_state(U.ta_url, LV_STATE_FOCUSED);
    lv_obj_clear_state(U.ta_key, LV_STATE_FOCUSED);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(U.kb, ta);
}

static lv_obj_t *mk_ta(lv_obj_t *p, const char *placeholder, int y, int max)
{
    lv_obj_t *ta = lv_textarea_create(p);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_obj_set_style_text_font(ta, APP_FONT_MEDIUM, 0);
    lv_obj_set_size(ta, 768, 48);
    lv_obj_set_pos(ta, 16, y);
    lv_obj_add_event_cb(ta, ta_focused, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void setup_open(lv_event_t *e)
{
    (void)e;
    if (U.setup) return;
    lv_obj_t *v = U.setup = lv_obj_create(U.root);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(v, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);         /* blocks the view underneath */
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = mk_label(v, APP_FONT_LARGE, APP_COLOR_TEXT, "Jellyfin server");
    lv_obj_set_pos(t, 16, 12);
    lv_obj_align(mk_btn(v, LV_SYMBOL_OK "  Save", 130, 44, 0x1B8F4A, setup_save_clicked),
                 LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_obj_align(mk_btn(v, "Cancel", 116, 44, C_BUTTON, setup_cancel_clicked),
                 LV_ALIGN_TOP_RIGHT, -154, 6);
    if (jf_configured()) {
        lv_obj_align(mk_btn(v, LV_SYMBOL_TRASH "  Forget", 130, 44, 0x8E2A2A,
                            setup_forget_clicked), LV_ALIGN_TOP_RIGHT, -282, 6);
    }

    char url[JF_URL_MAX];
    jf_get_url(url, sizeof(url));
    U.ta_url = mk_ta(v, "Server address, e.g. http://192.168.1.20:8096", 58, JF_URL_MAX - 1);
    lv_textarea_set_text(U.ta_url, url[0] ? url : "http://");
    char hint[8], ph[96];
    jf_key_hint(hint, sizeof(hint));
    if (hint[0]) {
        snprintf(ph, sizeof(ph), "Key saved (ends in %s). Type a new one to replace it.", hint);
    } else {
        snprintf(ph, sizeof(ph), "API key (Jellyfin Dashboard > API Keys)");
    }
    U.ta_key = mk_ta(v, ph, 112, JF_KEY_MAX - 1);
    lv_textarea_set_password_mode(U.ta_key, true);

    U.kb = lv_keyboard_create(v);
    lv_obj_set_size(U.kb, 800, 250);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(U.kb, kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(U.kb, kb_event, LV_EVENT_CANCEL, NULL);
    lv_obj_add_state(U.ta_url, LV_STATE_FOCUSED);
    lv_keyboard_set_textarea(U.kb, U.ta_url);
}

/* ---------- app ---------- */

static lv_obj_t *mk_round(lv_obj_t *p, const char *sym, int size, uint32_t color,
                          lv_event_cb_t cb, lv_obj_t **icon)
{
    lv_obj_t *b = mk_btn(p, sym, size, size, color, cb);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_font(l, size > 80 ? APP_FONT_LARGE : APP_FONT_MEDIUM, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4450), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(l, lv_color_hex(0x6B7680), LV_STATE_DISABLED);
    if (icon) *icon = l;
    return b;
}

static void jellyfin_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.root = parent;
    U.want_paused = -1;

    /* ----- main view ----- */
    lv_obj_t *m = U.main = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);

    U.art_box = lv_obj_create(m);
    lv_obj_remove_style_all(U.art_box);
    lv_obj_set_size(U.art_box, JF_ART_W, JF_ART_H);
    lv_obj_set_pos(U.art_box, ART_X, ART_Y);
    lv_obj_set_style_radius(U.art_box, 16, 0);
    lv_obj_set_style_clip_corner(U.art_box, true, 0);
    lv_obj_set_style_bg_color(U.art_box, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(U.art_box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(U.art_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    U.art_img = lv_img_create(U.art_box);
    lv_obj_add_flag(U.art_img, LV_OBJ_FLAG_HIDDEN);
    U.art_icon = mk_label(U.art_box, APP_FONT_HUGE, 0x5F6B77, LV_SYMBOL_AUDIO);
    lv_obj_center(U.art_icon);

    U.who = mk_label(m, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_label_set_long_mode(U.who, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.who, COL_W - 60);
    lv_obj_set_pos(U.who, COL_X, 26);

    U.title = mk_label(m, APP_FONT_LARGE, APP_COLOR_TEXT, "");
    lv_label_set_long_mode(U.title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(U.title, COL_W, 72);               /* two lines at most */
    lv_obj_set_pos(U.title, COL_X, 52);

    U.line2 = mk_label(m, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_long_mode(U.line2, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.line2, COL_W);
    lv_obj_set_pos(U.line2, COL_X, 128);

    U.line3 = mk_label(m, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");
    lv_label_set_long_mode(U.line3, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.line3, COL_W);
    lv_obj_set_pos(U.line3, COL_X, 158);

    U.badge = mk_label(m, LV_FONT_DEFAULT, 0xFFB74D, "");
    lv_obj_set_pos(U.badge, COL_X, 196);

    /* Session switcher (only with 2+ sessions), right of the badge. */
    U.sw_next = mk_round(m, LV_SYMBOL_RIGHT, 44, C_BUTTON, sw_next_clicked, NULL);
    lv_obj_set_pos(U.sw_next, COL_X + COL_W - 44, 184);
    U.sw_label = mk_label(m, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_set_width(U.sw_label, 64);
    lv_obj_set_style_text_align(U.sw_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(U.sw_label, COL_X + COL_W - 44 - 68, 198);
    U.sw_prev = mk_round(m, LV_SYMBOL_LEFT, 44, C_BUTTON, sw_prev_clicked, NULL);
    lv_obj_set_pos(U.sw_prev, COL_X + COL_W - 44 - 72 - 44, 184);

    /* Transport buttons, centred in the column. */
    int cx = COL_X + COL_W / 2, by = 244;
    U.btn_play = mk_round(m, LV_SYMBOL_PAUSE, 88, C_ACCENT, play_clicked, &U.play_icon);
    lv_obj_set_pos(U.btn_play, cx - 44, by);
    U.btn_prev = mk_round(m, LV_SYMBOL_PREV, 68, C_BUTTON, prev_clicked, NULL);
    lv_obj_set_pos(U.btn_prev, cx - 44 - 24 - 68, by + 10);
    U.btn_next = mk_round(m, LV_SYMBOL_NEXT, 68, C_BUTTON, next_clicked, NULL);
    lv_obj_set_pos(U.btn_next, cx + 44 + 24, by + 10);
    U.btn_stop = mk_round(m, LV_SYMBOL_STOP, 56, C_BUTTON, stop_clicked, NULL);
    lv_obj_set_pos(U.btn_stop, COL_X + COL_W - 56, by + 16);
    U.no_ctrl = mk_label(m, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");
    lv_label_set_long_mode(U.no_ctrl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(U.no_ctrl, COL_W);
    lv_obj_set_pos(U.no_ctrl, COL_X, by + 14);
    lv_obj_add_flag(U.no_ctrl, LV_OBJ_FLAG_HIDDEN);

    /* Progress bar across the bottom; drag it to seek. */
    U.slider = lv_slider_create(m);
    lv_slider_set_range(U.slider, 0, 1000);
    lv_obj_set_size(U.slider, 800 - 2 * ART_X - 24, 10);
    lv_obj_set_pos(U.slider, ART_X + 12, 352);
    lv_obj_set_ext_click_area(U.slider, 20);
    lv_obj_set_style_bg_color(U.slider, lv_color_hex(0x3A4450), LV_PART_MAIN);
    lv_obj_set_style_bg_color(U.slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(U.slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(U.slider, 6, LV_PART_KNOB);
    lv_obj_set_style_bg_color(U.slider, lv_color_hex(0x3A4450), LV_PART_INDICATOR | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(U.slider, LV_OPA_TRANSP, LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_add_event_cb(U.slider, slider_event, LV_EVENT_ALL, NULL);
    U.t_pos = mk_label(m, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_set_pos(U.t_pos, ART_X, 374);
    U.t_len = mk_label(m, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_align(U.t_len, LV_ALIGN_TOP_RIGHT, -ART_X, 374);

    /* ----- message view (not set up, errors, nothing playing) ----- */
    lv_obj_t *mv = U.msg_view = lv_obj_create(parent);
    lv_obj_remove_style_all(mv);
    lv_obj_set_size(mv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(mv, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mv, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(mv, 18, 0);
    lv_obj_clear_flag(mv, LV_OBJ_FLAG_SCROLLABLE);
    U.msg_icon = mk_label(mv, APP_FONT_HUGE, C_ACCENT, "");
    U.msg_text = mk_label(mv, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_obj_set_width(U.msg_text, 640);
    lv_label_set_long_mode(U.msg_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(U.msg_text, LV_TEXT_ALIGN_CENTER, 0);
    U.msg_btn = mk_btn(mv, LV_SYMBOL_SETTINGS "  Set up the server", 300, 52, 0x1E5A8C,
                       setup_open);

    /* Gear: server settings, always reachable (top right). */
    lv_obj_t *gear = mk_round(parent, LV_SYMBOL_SETTINGS, 44, C_BUTTON, setup_open, NULL);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -ART_X, 14);

    show_art("", NULL, 0, 0);
    jf_set_active(true);
    jf_snapshot(&U.snap);
    U.shown_version = U.snap.version + 1;               /* force the first draw */
    tick(NULL);
    U.timer = lv_timer_create(tick, 250, NULL);
}

static void jellyfin_destroy(void)
{
    jf_set_active(false);
    if (U.timer) lv_timer_del(U.timer);
    if (U.ta_key) lv_textarea_set_text(U.ta_key, "");
    memset(&U, 0, sizeof(U));                  /* the launcher deletes the objects */
}

const app_t jellyfin_app = {
    .name      = "Jellyfin",
    .symbol    = LV_SYMBOL_PLAY,
    .color_hex = C_ACCENT,
    .create    = jellyfin_create,
    .destroy   = jellyfin_destroy,
};
