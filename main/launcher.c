/*
 * Home screen: a grid of app tiles. Tapping a tile opens the app on its own
 * screen with a "Home" button in the top bar. A folder tile (app_t.folder)
 * opens a second grid; apps opened from there go back to the folder.
 *
 * Everything here runs in the LVGL task (event callbacks and lv_timers),
 * so no extra locking is needed.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "app_framework.h"
#include "launcher.h"
#include "clock_tools.h"
#include "net_time.h"
#include "photo_frame.h"
#include "settings.h"
#include "screenshot.h"
#include "sd_card.h"
#include "ticker.h"
#include "wifi_list.h"
#include "esp_err.h"

/* Backlight helpers from waveshare_rgb_lcd_port.c (declared here because that
 * header defines a static TAG that would warn as unused). */
esp_err_t wavesahre_rgb_lcd_bl_on(void);
esp_err_t wavesahre_rgb_lcd_bl_off(void);

/* ---- App registry: add new apps here ----
 * Order = default tile order (apps with pin_last, Settings, start last);
 * the user's own order is saved in settings, see order_load().
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
    &timers_app,
    &weather_app,
    &stonks_app,
    &photos_app,
    &files_app,
    &games_folder,
    &settings_app,
};
#define APP_COUNT (sizeof(s_apps) / sizeof(s_apps[0]))

#define BAR_H 64

static lv_obj_t   *s_home_scr;
static lv_obj_t   *s_home_clock;
static lv_obj_t   *s_home_wifi;
static lv_obj_t   *s_home_status;  /* next alarm / running timer */
static lv_obj_t   *s_home_grid;
static lv_obj_t   *s_home_ticker;  /* info ticker strip, or NULL */
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
    screenshot_attach(bar);   /* long-press: send the screen to the PC */
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

static lv_obj_t *build_grid(lv_obj_t *scr, const app_t *const *apps, size_t count,
                            lv_coord_t height, bool compact, const char *key);
static void edit_end(void);

static lv_obj_t *s_edit_grid;      /* grid being rearranged, or NULL */
static char      s_folder_key[16]; /* settings key of the open folder's order */
static bool      s_folder_reordered;
static void layout_home(void);

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
    if (s_edit_grid && lv_obj_get_screen(s_edit_grid) == s_folder_scr) edit_end();
    lv_scr_load(s_home_scr);
    if (s_folder_reordered) {
        s_folder_reordered = false;
        layout_home();            /* the folder tile's preview shows the new order */
    }
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
    snprintf(s_folder_key, sizeof(s_folder_key), "ord_%.11s", folder->name);
    build_grid(scr, folder->folder, folder_count(folder),
               lv_disp_get_ver_res(NULL) - BAR_H, false, s_folder_key);

    s_folder_scr = scr;
    s_folder = folder;
    lv_scr_load(scr);
}

/* ---------- tile order and rearranging ----------
 * Hold a tile to pick it up: it follows the finger, the other tiles make
 * room, and "Done" in the top bar ends rearranging (swipes still scroll,
 * taps open nothing until then). Each grid's order is saved as a settings
 * string of app names ("ord_home", "ord_<folder>"); apps missing from it
 * (new ones) go at the end. pin_last (Settings) only sets the starting
 * place; those tiles can be moved like any other.
 */
#define ORDER_MAX 400
#define GRID_MAX  32

static lv_obj_t  *s_edit_done;   /* "Done" button while rearranging */
static lv_obj_t  *s_drag_tile;   /* tile being dragged (left faded in place) */
static lv_obj_t  *s_drag_ghost;  /* copy of it on the top layer, under the finger */
static lv_point_t s_drag_ofs;    /* finger position inside the tile */

static void make_tile(lv_obj_t *grid, const app_t *app, lv_coord_t w, lv_coord_t h);

/* apps in the saved order; returns how many */
static size_t order_load(const char *key, const app_t *const *apps, size_t count,
                         const app_t **out)
{
    char saved[ORDER_MAX];
    bool used[GRID_MAX] = { false };
    size_t n = 0;

    if (count > GRID_MAX) count = GRID_MAX;
    settings_get_str(key, saved, sizeof(saved));
    char *save;
    for (char *name = strtok_r(saved, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
        for (size_t i = 0; i < count; i++) {
            if (!used[i] && strcmp(apps[i]->name, name) == 0) {
                out[n++] = apps[i];
                used[i] = true;
                break;
            }
        }
    }
    /* Two passes: the rest of the normal apps, then pinned-last ones. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            if (!used[i] && apps[i]->pin_last == (pass == 1)) out[n++] = apps[i];
        }
    }
    return n;
}

static void order_save(lv_obj_t *grid)
{
    char buf[ORDER_MAX], old[ORDER_MAX];
    size_t len = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(grid); i++) {
        const app_t *app = lv_obj_get_user_data(lv_obj_get_child(grid, i));
        if (!app) continue;
        int w = snprintf(buf + len, sizeof(buf) - len, "%s%s", len ? "," : "", app->name);
        if (w < 0 || (size_t)w >= sizeof(buf) - len) {
            buf[len] = '\0';
            break;
        }
        len += w;
    }
    const char *key = lv_obj_get_user_data(grid);
    if (!settings_get_str(key, old, sizeof(old)) || strcmp(old, buf) != 0) {
        settings_set_str(key, buf);
        if (key == s_folder_key) s_folder_reordered = true;
    }
}

/* Folder tile icon: a 2 x 2 preview of the apps inside, in their colours. */
static void draw_folder_preview(lv_obj_t *holder, const app_t *folder, int size)
{
    const int gap = 6, cell = (size - gap) / 2;
    char key[16];
    const app_t *apps[GRID_MAX];
    snprintf(key, sizeof(key), "ord_%.11s", folder->name);
    size_t n = order_load(key, folder->folder, folder_count(folder), apps);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = lv_obj_create(holder);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cell, cell);
        lv_obj_set_pos(c, (i % 2) * (cell + gap), (i / 2) * (cell + gap));
        lv_obj_set_style_radius(c, 7, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        if ((size_t)i < n) {
            lv_obj_set_style_bg_color(c, lv_color_hex(apps[i]->color_hex), 0);
            lv_obj_set_style_border_color(c, lv_color_white(), 0);
            lv_obj_set_style_border_width(c, 2, 0);
        } else {
            lv_obj_set_style_bg_color(c, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_20, 0);
        }
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void drag_start(lv_obj_t *tile, const lv_point_t *p)
{
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    s_drag_tile = tile;
    s_drag_ofs.x = p->x - a.x1;
    s_drag_ofs.y = p->y - a.y1;

    make_tile(lv_layer_top(), lv_obj_get_user_data(tile),
              lv_area_get_width(&a), lv_area_get_height(&a));
    s_drag_ghost = lv_obj_get_child(lv_layer_top(), -1);
    lv_obj_clear_flag(s_drag_ghost, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_shadow_width(s_drag_ghost, 30, 0);
    lv_obj_set_style_shadow_opa(s_drag_ghost, LV_OPA_70, 0);
    lv_obj_set_style_border_color(s_drag_ghost, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_drag_ghost, 3, 0);
    lv_obj_set_pos(s_drag_ghost, a.x1, a.y1);

    lv_obj_set_style_opa(tile, LV_OPA_20, 0);
    /* Moving the finger now drags the tile instead of scrolling the grid. */
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLL_CHAIN);
}

static void drag_move(const lv_point_t *p)
{
    lv_obj_set_pos(s_drag_ghost, p->x - s_drag_ofs.x, p->y - s_drag_ofs.y);

    /* Near the top or bottom edge: scroll to reach the rest of the grid. */
    lv_obj_t *grid = lv_obj_get_parent(s_drag_tile);
    lv_area_t g;
    lv_obj_get_coords(grid, &g);
    if (p->y < g.y1 + 50) {
        lv_obj_scroll_by_bounded(grid, 0, 10, LV_ANIM_OFF);
    } else if (p->y > g.y2 - 50) {
        lv_obj_scroll_by_bounded(grid, 0, -10, LV_ANIM_OFF);
    }

    /* Finger over another tile: take its place (the rest shift along). */
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(grid); i++) {
        lv_obj_t *t = lv_obj_get_child(grid, i);
        lv_area_t a;
        lv_obj_get_coords(t, &a);
        if (t != s_drag_tile && _lv_area_is_point_on(&a, p, 0)) {
            lv_obj_move_to_index(s_drag_tile, i);
            lv_obj_update_layout(grid);
            break;
        }
    }
}

static void drag_end(void)
{
    lv_obj_t *grid = lv_obj_get_parent(s_drag_tile);
    lv_obj_del(s_drag_ghost);
    lv_obj_set_style_opa(s_drag_tile, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_drag_tile, LV_OBJ_FLAG_SCROLL_CHAIN);
    s_drag_ghost = NULL;
    s_drag_tile = NULL;
    order_save(grid);
}

static void edit_set_borders(lv_obj_t *grid, bool on)
{
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(grid); i++) {
        lv_obj_t *t = lv_obj_get_child(grid, i);
        lv_obj_set_style_border_color(t, lv_color_white(), 0);
        lv_obj_set_style_border_opa(t, LV_OPA_60, 0);
        lv_obj_set_style_border_width(t, on ? 3 : 0, 0);
    }
}

static void edit_done_clicked(lv_event_t *e)
{
    (void)e;
    edit_end();
}

static void edit_begin(lv_obj_t *grid)
{
    if (s_edit_grid == grid) return;
    edit_end();
    s_edit_grid = grid;
    edit_set_borders(grid, true);

    /* Over the middle of the top bar (covers a folder's title). */
    lv_obj_t *bar = lv_obj_get_child(lv_obj_get_screen(grid), 0);
    s_edit_done = lv_btn_create(bar);
    lv_obj_set_size(s_edit_done, 170, 44);
    lv_obj_align(s_edit_done, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_edit_done, 22, 0);
    lv_obj_set_style_bg_color(s_edit_done, lv_color_hex(0x43A047), 0);
    lv_obj_add_event_cb(s_edit_done, edit_done_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_edit_done);
    lv_label_set_text(lbl, LV_SYMBOL_OK "  Done");
    lv_obj_set_style_text_font(lbl, APP_FONT_MEDIUM, 0);
    lv_obj_center(lbl);
}

static void edit_end(void)
{
    if (!s_edit_grid) return;
    if (s_drag_tile) drag_end();
    edit_set_borders(s_edit_grid, false);
    lv_obj_del_async(s_edit_done);   /* we may be inside its event */
    s_edit_done = NULL;
    s_edit_grid = NULL;
}

static void tile_event(lv_event_t *e)
{
    lv_obj_t *tile = lv_event_get_current_target(e);
    const app_t *app = lv_obj_get_user_data(tile);
    lv_point_t p = { 0, 0 };
    if (lv_indev_get_act()) lv_indev_get_point(lv_indev_get_act(), &p);

    switch (lv_event_get_code(e)) {
    case LV_EVENT_CLICKED:
        if (s_edit_grid) break;            /* rearranging: taps open nothing */
        if (app->folder) {
            open_folder(app);
        } else {
            launch_app(app);
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        edit_begin(lv_obj_get_parent(tile));
        if (!s_drag_tile) drag_start(tile, &p);
        break;
    case LV_EVENT_PRESSING:
        if (s_drag_tile == tile) drag_move(&p);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        if (s_drag_tile == tile) drag_end();
        break;
    default:
        break;
    }
}

/* ---------- home screen ---------- */

static void home_tick(lv_timer_t *t)
{
    (void)t;
    /* The ticker only scrolls while it can be seen. */
    ticker_set_running(lv_scr_act() == s_home_scr && !s_wake_catcher);
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

    char st[32];
    if (clock_tools_status(st, sizeof(st))) {
        lv_label_set_text(s_home_status, st);
        lv_obj_clear_flag(s_home_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_home_status, LV_OBJ_FLAG_HIDDEN);
    }
}

static void make_tile(lv_obj_t *grid, const app_t *app, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *tile = lv_btn_create(grid);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_radius(tile, 18, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(app->color_hex), 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(tile, 12, 0);
    lv_obj_set_user_data(tile, (void *)app);
    lv_obj_add_event_cb(tile, tile_event, LV_EVENT_ALL, NULL);

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

/* Tile grid below the top bar (home screen or a folder), in the order saved
 * under settings key `key` (kept as the grid's user data). It scrolls up and
 * down when the tiles don't fit. Compact = smaller tiles (5 per row) to make
 * room for the info ticker. */
static lv_obj_t *build_grid(lv_obj_t *scr, const app_t *const *apps, size_t count,
                            lv_coord_t height, bool compact, const char *key)
{
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), height);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, BAR_H);
    lv_obj_set_user_data(grid, (void *)key);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(grid, compact ? 18 : 28, 0);
    lv_obj_set_style_pad_ver(grid, compact ? 16 : 28, 0);
    lv_obj_set_style_pad_row(grid, compact ? 16 : 24, 0);
    lv_obj_set_style_pad_column(grid, compact ? 16 : 24, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(grid, lv_color_white(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(grid, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(grid, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(grid, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_ver(grid, 8, LV_PART_SCROLLBAR);

    const app_t *ordered[GRID_MAX];
    size_t n = order_load(key, apps, count, ordered);
    for (size_t i = 0; i < n; i++) {
        make_tile(grid, ordered[i], compact ? 140 : 160, compact ? 132 : 150);
    }
    return grid;
}

/* Home tiles, plus the ticker strip when it's switched on. */
static void layout_home(void)
{
    if (s_home_grid && s_edit_grid == s_home_grid) edit_end();
    if (s_home_grid) lv_obj_del(s_home_grid);
    if (s_home_ticker) lv_obj_del(s_home_ticker);
    s_home_ticker = NULL;

    bool ticker = settings_get_ticker();
    lv_coord_t h = lv_disp_get_ver_res(NULL) - BAR_H - (ticker ? TICKER_H : 0);
    s_home_grid = build_grid(s_home_scr, s_apps, APP_COUNT, h, ticker, "ord_home");
    if (ticker) {
        s_home_ticker = ticker_create(s_home_scr);
        ticker_set_running(lv_scr_act() == s_home_scr);
    }
}

void launcher_apply_ticker(void)
{
    if (s_home_scr) layout_home();
}

static void ticker_service_tick(lv_timer_t *t)
{
    (void)t;
    ticker_service();
}

static void gear_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_edit_grid) launch_app(&settings_app);
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

    /* Settings shortcut, so it's easy to reach wherever its tile was moved. */
    lv_obj_t *gear = lv_label_create(bar);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(gear, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(gear, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -130, 0);
    lv_obj_add_flag(gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(gear, 18);
    lv_obj_add_event_cb(gear, gear_clicked, LV_EVENT_CLICKED, NULL);

    s_home_wifi = lv_label_create(bar);
    lv_label_set_text(s_home_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_home_wifi, APP_FONT_MEDIUM, 0);
    lv_obj_align(s_home_wifi, LV_ALIGN_RIGHT_MID, -180, 0);

    s_home_status = lv_label_create(bar);
    lv_obj_set_style_text_font(s_home_status, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(s_home_status, lv_color_hex(0xFFB74D), 0);
    lv_label_set_text(s_home_status, "");
    lv_obj_align(s_home_status, LV_ALIGN_RIGHT_MID, -230, 0);
    lv_obj_add_flag(s_home_status, LV_OBJ_FLAG_HIDDEN);

    layout_home();

    lv_timer_create(home_tick, 500, NULL);
}

/* ---------- screen timeout ----------
 * The backlight is on/off only (CH422G EXIO2), so "sleep" = backlight off.
 * While asleep, an invisible full-screen object on the top layer catches
 * the waking tap so it doesn't also press whatever is underneath.
 */

static bool s_saver;                /* the catcher holds the photo screensaver */

static void screen_wake(lv_event_t *e)
{
    (void)e;
    if (!s_wake_catcher) {
        return;
    }
    if (s_saver) {
        photo_frame_close();
        s_saver = false;
    }
    wavesahre_rgb_lcd_bl_on();
    /* Async: we're inside this object's own event. */
    lv_obj_del_async(s_wake_catcher);
    s_wake_catcher = NULL;
}

void launcher_wake(void)
{
    lv_disp_trig_activity(NULL);
    if (!s_wake_catcher) return;
    if (s_saver) {
        photo_frame_close();
        s_saver = false;
    }
    wavesahre_rgb_lcd_bl_on();
    lv_obj_del(s_wake_catcher);
    s_wake_catcher = NULL;
}

static void screen_timeout_tick(lv_timer_t *t)
{
    (void)t;
    /* A screensaver with no pictures to show turns the screen off instead. */
    if (s_saver && (photo_frame_state() == PF_EMPTY || photo_frame_state() == PF_NO_CARD)) {
        photo_frame_close();
        lv_obj_clean(s_wake_catcher);
        s_saver = false;
        wavesahre_rgb_lcd_bl_off();
        return;
    }
    int minutes = settings_get_screen_timeout_min();
    if (s_wake_catcher || minutes == 0 || s_current_app == &photos_app || clock_tools_alerting()) {
        return;                          /* the Photos app is a photo frame: stay on */
    }
    if (lv_disp_get_inactive_time(NULL) < (uint32_t)minutes * 60000U) {
        return;
    }

    s_wake_catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wake_catcher);
    lv_obj_set_size(s_wake_catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wake_catcher, screen_wake, LV_EVENT_PRESSED, NULL);
    if (settings_get_u8("scr_saver", 0) == 1 && sd_card_mounted()) {
        photo_frame_open(s_wake_catcher, 800, 480);   /* slideshow instead of dark */
        s_saver = true;
    } else {
        wavesahre_rgb_lcd_bl_off();
    }
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

/* ---------- messages from background tasks ----------
 * A rounded banner near the bottom of the screen, above everything, that
 * goes away by itself (tap to dismiss sooner).
 */

static lv_obj_t   *s_toast;
static lv_timer_t *s_toast_timer;

static void toast_close(lv_timer_t *t)
{
    (void)t;
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    s_toast_timer = NULL;   /* one-shot: LVGL deletes it after this */
}

static void toast_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_del_async(s_toast);
    s_toast = NULL;
}

void launcher_toast(const char *text, uint32_t ms)
{
    if (s_toast) lv_obj_del(s_toast);
    if (s_toast_timer) lv_timer_del(s_toast_timer);
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_toast, 740, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(s_toast, 16, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x263238), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x42A5F5), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_pad_hor(s_toast, 24, 0);
    lv_obj_set_style_pad_ver(s_toast, 16, 0);
    lv_obj_set_style_shadow_width(s_toast, 24, 0);
    lv_obj_set_style_shadow_opa(s_toast, LV_OPA_50, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_toast, toast_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(s_toast);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 692, 0);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(APP_COLOR_TEXT), 0);

    s_toast_timer = lv_timer_create(toast_close, ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

static void notice_tick(lv_timer_t *t)
{
    (void)t;
    char msg[160];
    if (sd_card_take_notice(msg, sizeof(msg))) {
        /* Problems stay up longer so there's time to read them. */
        launcher_toast(msg, strstr(msg, "not imported") ? 15000 : 6000);
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
    wifi_list_init();
    net_time_start();
    build_home();
    lv_scr_load(s_home_scr);
    lv_timer_create(screen_timeout_tick, 1000, NULL);
    lv_timer_create(fps_init_tick, 5, NULL);
    lv_timer_create(ticker_service_tick, 1000, NULL);
    lv_timer_create(notice_tick, 500, NULL);
    clock_tools_start();
}
