/*
 * Shared board-game helpers. See game_common.h.
 */
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "app_framework.h"
#include "game_common.h"

static const char *TAG = "game";

#define C_SELECTED  0x42A5F5
#define C_LAST      0xF6E05E
#define C_ALERT     0xE53935
#define C_DOT       0x1B1B1B

const char *const game_level_names[GAME_LEVELS] = { "Easy", "Medium", "Hard", "Expert" };

/* ======================= board widget ======================= */

static struct {
    lv_obj_t        *obj;
    game_board_cfg_t cfg;
    bool             flipped;
    uint64_t         selected, targets, last, alert;
} s_board;

/* Screen position (0..7 from top-left) -> board row/col. */
static void screen_to_board(int sr, int sc, int *row, int *col)
{
    if (s_board.flipped) {
        *row = sr;
        *col = 7 - sc;
    } else {
        *row = 7 - sr;
        *col = sc;
    }
}

static void fill(lv_draw_ctx_t *ctx, const lv_area_t *a, uint32_t color,
                 lv_opa_t opa, lv_coord_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(ctx, &d, a);
}

static void board_draw_event(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    lv_obj_get_coords(s_board.obj, &box);

    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = LV_FONT_DEFAULT;

    for (int sr = 0; sr < 8; sr++) {
        for (int sc = 0; sc < 8; sc++) {
            int row, col;
            screen_to_board(sr, sc, &row, &col);
            int sq = row * 8 + col;
            bool dark = ((row + col) & 1) == 0;

            lv_area_t a = {
                .x1 = box.x1 + sc * GAME_SQ, .y1 = box.y1 + sr * GAME_SQ,
                .x2 = box.x1 + sc * GAME_SQ + GAME_SQ - 1,
                .y2 = box.y1 + sr * GAME_SQ + GAME_SQ - 1,
            };
            fill(ctx, &a, dark ? s_board.cfg.dark : s_board.cfg.light, LV_OPA_COVER, 0);

            if (s_board.last & GAME_BIT(sq))     fill(ctx, &a, C_LAST, LV_OPA_50, 0);
            if (s_board.selected & GAME_BIT(sq)) fill(ctx, &a, C_SELECTED, LV_OPA_60, 0);
            if (s_board.alert & GAME_BIT(sq))    fill(ctx, &a, C_ALERT, LV_OPA_70, 0);

            if (s_board.cfg.coords) {
                char t[2] = { 0, 0 };
                ld.color = lv_color_hex(dark ? s_board.cfg.light : s_board.cfg.dark);
                if (sc == 0) {
                    t[0] = (char)('1' + row);
                    lv_area_t la = { a.x1 + 2, a.y1 + 1, a.x1 + 14, a.y1 + 17 };
                    lv_draw_label(ctx, &ld, &la, t, NULL);
                }
                if (sr == 7) {
                    t[0] = (char)('a' + col);
                    lv_area_t la = { a.x2 - 10, a.y2 - 17, a.x2, a.y2 };
                    lv_draw_label(ctx, &ld, &la, t, NULL);
                }
            }

            if (s_board.cfg.draw) {
                s_board.cfg.draw(ctx, &a, row, col);
            }

            if (s_board.targets & GAME_BIT(sq)) {
                lv_area_t d = { a.x1 + 17, a.y1 + 17, a.x2 - 17, a.y2 - 17 };
                fill(ctx, &d, C_DOT, LV_OPA_50, LV_RADIUS_CIRCLE);
            }
        }
    }
}

static void board_click_event(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_board.cfg.tap) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t box;
    lv_obj_get_coords(s_board.obj, &box);
    int sc = (p.x - box.x1) / GAME_SQ;
    int sr = (p.y - box.y1) / GAME_SQ;
    if (sc < 0 || sc > 7 || sr < 0 || sr > 7) {
        return;
    }
    int row, col;
    screen_to_board(sr, sc, &row, &col);
    s_board.cfg.tap(row, col);
}

static void board_delete_event(lv_event_t *e)
{
    (void)e;
    s_board.obj = NULL;
}

lv_obj_t *game_board_create(lv_obj_t *parent, const game_board_cfg_t *cfg)
{
    s_board.cfg = *cfg;
    s_board.flipped = false;
    s_board.selected = s_board.targets = s_board.last = s_board.alert = 0;

    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, GAME_BOARD_PX, GAME_BOARD_PX);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, board_draw_event, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o, board_click_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(o, board_delete_event, LV_EVENT_DELETE, NULL);
    s_board.obj = o;
    return o;
}

void game_board_set_flipped(bool flipped)
{
    s_board.flipped = flipped;
    if (s_board.obj) lv_obj_invalidate(s_board.obj);
}

void game_board_set_marks(uint64_t selected, uint64_t targets,
                          uint64_t last, uint64_t alert)
{
    s_board.selected = selected;
    s_board.targets = targets;
    s_board.last = last;
    s_board.alert = alert;
    if (s_board.obj) lv_obj_invalidate(s_board.obj);
}

void game_board_draw_img(lv_draw_ctx_t *ctx, const lv_area_t *area,
                         const lv_img_dsc_t *img)
{
    lv_draw_img_dsc_t d;
    lv_draw_img_dsc_init(&d);
    lv_coord_t w = img->header.w, h = img->header.h;
    lv_area_t a;
    a.x1 = area->x1 + (lv_area_get_width(area) - w) / 2;
    a.y1 = area->y1 + (lv_area_get_height(area) - h) / 2;
    a.x2 = a.x1 + w - 1;
    a.y2 = a.y1 + h - 1;
    lv_draw_img(ctx, &d, &a, img);
}

/* ======================= background worker ======================= */

static int      s_busy;           /* accessed with __atomic */
static int64_t  s_last_yield_us;

typedef struct {
    void (*fn)(void *arg);
    void *arg;
} job_t;
static job_t s_job;

static void worker_task(void *p)
{
    (void)p;
    s_last_yield_us = esp_timer_get_time();
    s_job.fn(s_job.arg);
    __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

bool game_worker_start(void (*fn)(void *arg), void *arg)
{
    if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) {
        return false;
    }
    s_job.fn = fn;
    s_job.arg = arg;
    __atomic_store_n(&s_busy, 1, __ATOMIC_RELEASE);
    /* Priority 1 (below LVGL's 2) on core 0, away from the LVGL task. */
    if (xTaskCreatePinnedToCore(worker_task, "game_ai", 16 * 1024, NULL, 1,
                                NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Couldn't start the worker task");
        __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

int64_t game_time_us(void)  { return esp_timer_get_time(); }
uint32_t game_random(void)  { return esp_random(); }

bool game_worker_busy(void)
{
    return __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE) != 0;
}

void game_worker_yield(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_last_yield_us > 50000) {
        vTaskDelay(1);
        s_last_yield_us = esp_timer_get_time();
    }
}

/* ======================= UI helpers ======================= */

static lv_obj_t *s_dialog;

static void dialog_delete_event(lv_event_t *e)
{
    (void)e;
    s_dialog = NULL;
}

lv_obj_t *game_dialog_open(lv_obj_t *parent, const char *title)
{
    game_dialog_close();

    /* Dim backdrop that also blocks taps on the board underneath. */
    lv_obj_t *bg = lv_obj_create(parent);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, dialog_delete_event, LV_EVENT_DELETE, NULL);
    s_dialog = bg;

    lv_obj_t *box = lv_obj_create(bg);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3A4652), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 20, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(box);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(APP_COLOR_TEXT), 0);

    lv_obj_t *row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return row;
}

lv_obj_t *game_dialog_button(lv_obj_t *row, const char *text,
                             lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_set_size(b, text ? LV_SIZE_CONTENT : 84, 72);
    lv_obj_set_style_min_width(b, 120, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    if (text) {
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
        lv_obj_center(l);
    }
    return b;
}

void game_dialog_close(void)
{
    if (s_dialog) {
        /* Async: usually called from a button inside the dialog. */
        lv_obj_del_async(s_dialog);
        s_dialog = NULL;
    }
}

lv_obj_t *game_button(lv_obj_t *parent, const char *text, lv_coord_t w,
                      lv_coord_t h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *game_level_selector(lv_obj_t *parent, int level, lv_event_cb_t cb)
{
    static const char *map[] = { "Easy", "Medium", "Hard", "Expert", "" };

    lv_obj_t *m = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(m, map);
    lv_btnmatrix_set_btn_ctrl_all(m, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(m, true);
    if (level >= 0 && level < GAME_LEVELS) {
        lv_btnmatrix_set_btn_ctrl(m, level, LV_BTNMATRIX_CTRL_CHECKED);
    }
    lv_obj_set_size(m, GAME_PANEL_W, 56);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 8, 0);
    lv_obj_set_style_text_font(m, APP_FONT_MEDIUM, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_hex(APP_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 12, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x2A333C), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x42A5F5), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, lv_color_hex(0x0D1B2A),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return m;
}

/* ======================= swipe input ======================= */

#define SWIPE_PX 36

static struct {
    void      (*cb)(game_dir_t dir);
    bool        repeat, fired;
    lv_point_t  anchor;
} s_swipe;

static void swipe_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_swipe.cb) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        s_swipe.anchor = p;
        s_swipe.fired = false;
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (!s_swipe.fired) s_swipe.cb(GAME_DIR_TAP);
        return;
    }
    if (s_swipe.fired && !s_swipe.repeat) return;

    int dx = p.x - s_swipe.anchor.x, dy = p.y - s_swipe.anchor.y;
    if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) return;
    game_dir_t dir = abs(dx) > abs(dy) ? (dx > 0 ? GAME_DIR_RIGHT : GAME_DIR_LEFT)
                                       : (dy > 0 ? GAME_DIR_DOWN : GAME_DIR_UP);
    s_swipe.anchor = p;
    s_swipe.fired = true;
    s_swipe.cb(dir);
}

lv_obj_t *game_swipe_layer(lv_obj_t *parent, bool repeat, void (*cb)(game_dir_t dir))
{
    s_swipe.cb = cb;
    s_swipe.repeat = repeat;
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(o, swipe_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(o, swipe_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(o, swipe_event, LV_EVENT_RELEASED, NULL);
    return o;
}
