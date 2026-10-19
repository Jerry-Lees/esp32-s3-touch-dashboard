/*
 * Home screen: a grid of app tiles. Tapping a tile opens the app on its own
 * screen with a "Home" button in the top bar. A folder tile (app_t.folder)
 * opens a second grid; apps opened from there go back to the folder.
 *
 * Everything here runs in the LVGL task (event callbacks and lv_timers),
 * so no extra locking is needed.
 */
#include <stdio.h>
#include <time.h>

#include "lvgl.h"
#include "app_framework.h"
#include "launcher.h"
#include "net_time.h"
#include "settings.h"
#include "esp_err.h"

/* Backlight helpers from waveshare_rgb_lcd_port.c (declared here because that
 * header defines a static TAG that would warn as unused). */
esp_err_t wavesahre_rgb_lcd_bl_on(void);
esp_err_t wavesahre_rgb_lcd_bl_off(void);

/* ---- App registry: add new apps here ----
 * Order = tile order, except apps with pin_last (Settings) always go last.
 * Folder contents are NULL-terminated lists. */
static const app_t *const s_games[] = {
    &chess_app,
    &checkers_app,
    &flappy_app,
    &snake_app,
    &g2048_app,
    NULL,
};

static const app_t games_folder = {
    .name      = "Games",
    .symbol    = LV_SYMBOL_DIRECTORY,
    .color_hex = 0x5E35B1,
    .folder    = s_games,
};

static const app_t *const s_apps[] = {
    &world_clock_app,
    &weather_app,
    &games_folder,
    &settings_app,
};
#define APP_COUNT (sizeof(s_apps) / sizeof(s_apps[0]))

#define BAR_H 64

static lv_obj_t   *s_home_scr;
static lv_obj_t   *s_home_clock;
static lv_obj_t   *s_home_wifi;
static lv_obj_t   *s_folder_scr;    /* open folder's screen, or NULL */
static const app_t *s_folder;       /* open folder, or NULL */
static lv_obj_t   *s_app_scr;
static const app_t *s_current_app;
static lv_obj_t   *s_wake_catcher;  /* non-NULL while the screen is asleep */

/* ---------- helpers ---------- */

static lv_obj_t *make_top_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 16, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static void make_back_button(lv_obj_t *bar, const char *to, lv_event_cb_t cb)
{
    lv_obj_t *back = lv_btn_create(bar);
    lv_obj_set_size(back, 130, 44);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_add_event_cb(back, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text_fmt(back_lbl, LV_SYMBOL_LEFT "  %s", to);
    lv_obj_set_style_text_font(back_lbl, APP_FONT_MEDIUM, 0);
    lv_obj_center(back_lbl);
}

static void make_title(lv_obj_t *bar, const char *text)
{
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
}

static void build_grid(lv_obj_t *scr, const app_t *const *apps, size_t count);

/* ---------- app screens ---------- */

static void back_clicked(lv_event_t *e)
{
    (void)e;
    if (s_current_app && s_current_app->destroy) {
        s_current_app->destroy();
    }
    s_current_app = NULL;

    lv_scr_load(s_folder_scr ? s_folder_scr : s_home_scr);
    if (s_app_scr) {
        /* Async: we're inside an event from a child of this screen. */
        lv_obj_del_async(s_app_scr);
        s_app_scr = NULL;
    }
}

static void launch_app(const app_t *app)
{
    lv_obj_t *scr = make_screen();

    lv_obj_t *bar = make_top_bar(scr);
    make_back_button(bar, s_folder ? s_folder->name : "Home", back_clicked);
    make_title(bar, app->name);

    /* Area below the bar that belongs to the app. */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, LV_PCT(100), lv_disp_get_ver_res(NULL) - BAR_H);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, BAR_H);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    s_app_scr = scr;
    s_current_app = app;
    app->create(content);

    lv_scr_load(scr);
}

/* ---------- folder screens ---------- */

static void folder_back_clicked(lv_event_t *e)
{
    (void)e;
    lv_scr_load(s_home_scr);
    if (s_folder_scr) {
        lv_obj_del_async(s_folder_scr);   /* we're inside its event */
        s_folder_scr = NULL;
    }
    s_folder = NULL;
}

static size_t folder_count(const app_t *folder)
{
    size_t n = 0;
    while (folder->folder[n]) n++;
    return n;
}

static void open_folder(const app_t *folder)
{
    lv_obj_t *scr = make_screen();
    lv_obj_t *bar = make_top_bar(scr);
    make_back_button(bar, "Home", folder_back_clicked);
    make_title(bar, folder->name);
    build_grid(scr, folder->folder, folder_count(folder));

    s_folder_scr = scr;
    s_folder = folder;
    lv_scr_load(scr);
}

/* Folder tile icon: a 2 x 2 preview of the apps inside, in their colours. */
static void draw_folder_preview(lv_obj_t *holder, const app_t *folder, int size)
{
    const int gap = 6, cell = (size - gap) / 2;
    size_t n = folder_count(folder);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = lv_obj_create(holder);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cell, cell);
        lv_obj_set_pos(c, (i % 2) * (cell + gap), (i / 2) * (cell + gap));
        lv_obj_set_style_radius(c, 7, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        if ((size_t)i < n) {
            lv_obj_set_style_bg_color(c, lv_color_hex(folder->folder[i]->color_hex), 0);
            lv_obj_set_style_border_color(c, lv_color_white(), 0);
            lv_obj_set_style_border_width(c, 2, 0);
        } else {
            lv_obj_set_style_bg_color(c, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_20, 0);
        }
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void tile_clicked(lv_event_t *e)
{
    const app_t *app = (const app_t *)lv_event_get_user_data(e);
    if (app->folder) {
        open_folder(app);
    } else {
        launch_app(app);
    }
}

/* ---------- home screen ---------- */

static void home_tick(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != s_home_scr) {
        return;
    }

    lv_obj_set_style_text_color(
        s_home_wifi,
        lv_color_hex(net_time_wifi_connected() ? 0x4CAF50 : 0x5F6B77), 0);

    if (net_time_is_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[24];
        if (settings_get_24h()) {
            strftime(buf, sizeof(buf), "%H:%M", &tm);
            lv_label_set_text(s_home_clock, buf);
        } else {
            strftime(buf, sizeof(buf), "%I:%M %p", &tm);
            lv_label_set_text(s_home_clock, buf[0] == '0' ? buf + 1 : buf);
        }
    } else {
        lv_label_set_text(s_home_clock, "--:--");
    }
}

static void make_tile(lv_obj_t *grid, const app_t *app)
{
    lv_obj_t *tile = lv_btn_create(grid);
    lv_obj_set_size(tile, 160, 150);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(app->color_hex), 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 12, 0);
    lv_obj_add_event_cb(tile, tile_clicked, LV_EVENT_CLICKED, (void *)app);

    if (app->draw_icon || app->folder) {
        lv_obj_t *holder = lv_obj_create(tile);
        lv_obj_remove_style_all(holder);
        lv_obj_set_size(holder, 64, 64);
        lv_obj_clear_flag(holder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        if (app->draw_icon) {
            app->draw_icon(holder, 64);
        } else {
            draw_folder_preview(holder, app, 64);
        }
    } else {
        lv_obj_t *icon = lv_label_create(tile);
        lv_label_set_text(icon, app->symbol);
        lv_obj_set_style_text_font(icon, APP_FONT_HUGE, 0);
    }

    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, app->name);
    lv_obj_set_style_text_font(name, APP_FONT_MEDIUM, 0);
}

/* Tile grid below the top bar (home screen or a folder). */
static void build_grid(lv_obj_t *scr, const app_t *const *apps, size_t count)
{
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), lv_disp_get_ver_res(NULL) - BAR_H);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, BAR_H);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(grid, 28, 0);
    lv_obj_set_style_pad_row(grid, 24, 0);
    lv_obj_set_style_pad_column(grid, 24, 0);

    /* Two passes: normal apps first, then pinned-last apps (Settings). */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if (apps[i]->pin_last == (pass == 1)) {
                make_tile(grid, apps[i]);
            }
        }
    }
}

static void build_home(void)
{
    s_home_scr = make_screen();

    lv_obj_t *bar = make_top_bar(s_home_scr);

    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, LV_SYMBOL_HOME "  Home");
    lv_obj_set_style_text_font(title, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    s_home_clock = lv_label_create(bar);
    lv_obj_set_style_text_font(s_home_clock, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(s_home_clock, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_label_set_text(s_home_clock, "--:--");
    lv_obj_align(s_home_clock, LV_ALIGN_RIGHT_MID, 0, 0);

    s_home_wifi = lv_label_create(bar);
    lv_label_set_text(s_home_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_home_wifi, APP_FONT_MEDIUM, 0);
    lv_obj_align(s_home_wifi, LV_ALIGN_RIGHT_MID, -130, 0);

    build_grid(s_home_scr, s_apps, APP_COUNT);

    lv_timer_create(home_tick, 500, NULL);
}

/* ---------- screen timeout ----------
 * The backlight is on/off only (CH422G EXIO2), so "sleep" = backlight off.
 * While asleep, an invisible full-screen object on the top layer catches
 * the waking tap so it doesn't also press whatever is underneath.
 */

static void screen_wake(lv_event_t *e)
{
    (void)e;
    if (!s_wake_catcher) {
        return;
    }
    wavesahre_rgb_lcd_bl_on();
    /* Async: we're inside this object's own event. */
    lv_obj_del_async(s_wake_catcher);
    s_wake_catcher = NULL;
}

static void screen_timeout_tick(lv_timer_t *t)
{
    (void)t;
    int minutes = settings_get_screen_timeout_min();
    if (s_wake_catcher || minutes == 0) {
        return;
    }
    if (lv_disp_get_inactive_time(NULL) < (uint32_t)minutes * 60000U) {
        return;
    }

    s_wake_catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_catcher);
    lv_obj_set_size(s_wake_catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake_catcher, screen_wake, LV_EVENT_PRESSED, NULL);
    wavesahre_rgb_lcd_bl_off();
}

/* ---------- FPS meter ----------
 * LVGL's perf monitor (enabled in menuconfig) is a label it creates on the
 * system layer at the first screen refresh. It can't be turned off at run
 * time, so we hide that label instead.
 */

void launcher_apply_fps(void)
{
    lv_obj_t *sys = lv_layer_sys();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(sys); i++) {
        lv_obj_t *o = lv_obj_get_child(sys, i);
        if (lv_obj_check_type(o, &lv_label_class)) {
            if (settings_get_show_fps()) {
                lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

/* Runs until the meter exists, then applies the setting once. */
static void fps_init_tick(lv_timer_t *t)
{
    if (lv_obj_get_child_cnt(lv_layer_sys()) > 0) {
        launcher_apply_fps();
        lv_timer_del(t);
    }
}

void launcher_start(void)
{
    settings_init();   /* also initializes NVS, before Wi-Fi needs it */
    net_time_start();
    build_home();
    lv_scr_load(s_home_scr);
    lv_timer_create(screen_timeout_tick, 1000, NULL);
    lv_timer_create(fps_init_tick, 5, NULL);
}
