#pragma once
/*
 * Shared pieces for board games (Chess, Checkers):
 *   - an 8x8 board widget that draws itself and reports taps,
 *   - a background worker so the computer can think without freezing the UI,
 *   - a simple modal dialog and the side-panel controls,
 *   - the piece images from game_images.c.
 *
 * Board coordinates: row 0..7 from the bottom (row 0 = rank 1 in chess),
 * col 0..7 from the left. Square index = row * 8 + col.
 */
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "game_sys.h"

#define GAME_SQ          50                 /* square size in pixels */
#define GAME_BOARD_PX    (GAME_SQ * 8)
#define GAME_PANEL_X     (GAME_BOARD_PX + 24)
#define GAME_PANEL_W     (800 - GAME_PANEL_X - 12)

#define GAME_LEVELS      4                  /* Easy, Medium, Hard, Expert */

#define GAME_BIT(sq)     (1ULL << (sq))

/* ---------- board widget ---------- */

/* Draws the piece (if any) on one square. `area` is the square's screen area. */
typedef void (*game_board_draw_cb_t)(lv_draw_ctx_t *ctx, const lv_area_t *area,
                                     int row, int col);
typedef void (*game_board_tap_cb_t)(int row, int col);

typedef struct {
    uint32_t light, dark;     /* square colours (row 0, col 0 is dark) */
    bool     coords;          /* draw a-h / 1-8 labels */
    game_board_draw_cb_t draw;
    game_board_tap_cb_t  tap;
} game_board_cfg_t;

/* Only one board exists at a time (only one app is open). */
lv_obj_t *game_board_create(lv_obj_t *parent, const game_board_cfg_t *cfg);
/* Flipped = row 7 at the bottom (used when the player has the "top" side). */
void game_board_set_flipped(bool flipped);
/* Bit masks of squares: selected (blue), targets (dots), last move
 * (yellow), alert (red, e.g. king in check). Redraws the board. */
void game_board_set_marks(uint64_t selected, uint64_t targets,
                          uint64_t last, uint64_t alert);
/* Draws an image centred in a square area. */
void game_board_draw_img(lv_draw_ctx_t *ctx, const lv_area_t *area,
                         const lv_img_dsc_t *img);

/* ---------- background worker ----------
 * Runs one job at a time in its own FreeRTOS task on core 0 (LVGL runs on
 * core 1). The job must NOT call LVGL; it writes its result into `arg`.
 * Poll game_worker_busy() from an lv_timer; once it returns false the
 * result is safe to read. */
bool game_worker_start(void (*fn)(void *arg), void *arg);
bool game_worker_busy(void);

/* ---------- UI helpers ---------- */

/* Modal dialog over `parent`. Returns the row to add buttons to. */
lv_obj_t *game_dialog_open(lv_obj_t *parent, const char *title);
/* Adds a button; text may be NULL if the caller adds its own content. */
lv_obj_t *game_dialog_button(lv_obj_t *row, const char *text,
                             lv_event_cb_t cb, void *user_data);
void game_dialog_close(void);

/* Rounded button with a text label. */
lv_obj_t *game_button(lv_obj_t *parent, const char *text, lv_coord_t w,
                      lv_coord_t h, lv_event_cb_t cb);
/* Easy / Medium / Hard / Expert selector. Calls cb on change; read the new
 * level with lv_btnmatrix_get_selected_btn(lv_event_get_target(e)). */
lv_obj_t *game_level_selector(lv_obj_t *parent, int level, lv_event_cb_t cb);
extern const char *const game_level_names[GAME_LEVELS];

/* ---------- images (game_images.c) ---------- */
extern const lv_img_dsc_t img_chess_w_king, img_chess_w_queen, img_chess_w_rook,
                          img_chess_w_bishop, img_chess_w_knight, img_chess_w_pawn;
extern const lv_img_dsc_t img_chess_b_king, img_chess_b_queen, img_chess_b_rook,
                          img_chess_b_bishop, img_chess_b_knight, img_chess_b_pawn;
extern const lv_img_dsc_t img_game_crown;
