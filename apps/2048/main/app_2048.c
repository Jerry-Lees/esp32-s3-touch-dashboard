/*
 * 2048: swipe to slide all tiles; two equal tiles that collide merge.
 *
 * The game itself is the `M` struct (plain numbers in a dash_keep block,
 * kept while the app is closed, so leaving and coming back resumes the game). The UI is one
 * LVGL object per tile. A move updates `M` at once, slides the tile
 * objects with lv_anim, and a short one-shot timer then removes the merged
 * tiles, "pops" the new values and shows the newly spawned tile.
 * The best score is saved with dash_set_int.
 *
 * An SD card app: tools/build_app.sh apps/2048
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "dash_style.h"
#include "dashboard_app.h"
#include "game_common.h"

#define N        4
#define CELL     86
#define GAP      10
#define BOARD_PX (N * CELL + (N + 1) * GAP)
#define BOARD_X  30
#define BOARD_Y  ((416 - BOARD_PX) / 2)
#define PANEL_X  (BOARD_X + BOARD_PX + 40)
#define PANEL_W  (800 - PANEL_X - 30)

#define SLIDE_MS 100

#define C_PAGE   0xFAF8EF
#define C_BOARD  0xBBADA0
#define C_EMPTY  0xCDC1B4
#define C_TEXT   0x776E65

/* The game (a dash_keep block, so it survives closing the app). */
typedef struct {
    uint32_t v[N * N];            /* 0 = empty; row 0 is the top */
    int      score, best;
    bool     started, won;        /* won = reached 2048 (asked once) */
} game_t;
static game_t *s_game;
#define M (*s_game)

/* The UI (only while the app is open). */
static struct {
    lv_obj_t  *parent, *board, *score_val, *best_val;
    lv_obj_t  *tile[N * N];       /* tile object on each cell, or NULL */
    lv_obj_t  *doomed[N * N];     /* tiles merged away, deleted after sliding */
    int        ndoomed;
    bool       popped[N * N];     /* cells whose value doubled this move */
    int        spawned;           /* cell of the new tile, or -1 */
    bool       busy;              /* a move is animating */
    int        pending;           /* swipe that arrived meanwhile, or -1 */
    int        saved_best;
    lv_timer_t *finish;
} U;

/* ---------- tiles ---------- */

static uint32_t tile_color(uint32_t v)
{
    switch (v) {
    case 2:    return 0xEEE4DA;
    case 4:    return 0xEDE0C8;
    case 8:    return 0xF2B179;
    case 16:   return 0xF59563;
    case 32:   return 0xF67C5F;
    case 64:   return 0xF65E3B;
    case 128:  return 0xEDCF72;
    case 256:  return 0xEDCC61;
    case 512:  return 0xEDC850;
    case 1024: return 0xEDC53F;
    case 2048: return 0xEDC22E;
    default:   return 0x3C3A32;
    }
}

static int cell_x(int cell) { return GAP + (cell % N) * (CELL + GAP); }
static int cell_y(int cell) { return GAP + (cell / N) * (CELL + GAP); }

static void tile_set_value(lv_obj_t *t, uint32_t v)
{
    lv_obj_set_style_bg_color(t, lv_color_hex(tile_color(v)), 0);
    lv_obj_t *l = lv_obj_get_child(t, 0);
    lv_label_set_text_fmt(l, "%" PRIu32, v);
    lv_obj_set_style_text_color(l, lv_color_hex(v <= 4 ? C_TEXT : 0xF9F6F2), 0);
    lv_obj_set_style_text_font(l, v < 100 ? APP_FONT_HUGE
                                  : v < 10000 ? APP_FONT_LARGE : APP_FONT_MEDIUM, 0);
}

/* Animation: size `v`, kept centred on the tile's cell (stored in user data). */
static void scale_cb(void *var, int32_t v)
{
    lv_obj_t *t = var;
    int cell = (int)(intptr_t)lv_obj_get_user_data(t);
    lv_obj_set_size(t, v, v);
    lv_obj_set_pos(t, cell_x(cell) + (CELL - v) / 2, cell_y(cell) + (CELL - v) / 2);
}

static void animate_scale(lv_obj_t *t, int from, int to, int ms, bool bounce)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, scale_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    if (bounce) {
        lv_anim_set_playback_time(&a, ms);
    } else {
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    }
    lv_anim_start(&a);
}

static lv_obj_t *tile_create(int cell, uint32_t v, bool appear)
{
    lv_obj_t *t = lv_obj_create(U.board);
    lv_obj_remove_style_all(t);
    lv_obj_set_style_radius(t, 6, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(t);
    lv_obj_center(l);
    tile_set_value(t, v);
    lv_obj_set_user_data(t, (void *)(intptr_t)cell);
    lv_obj_set_size(t, CELL, CELL);
    lv_obj_set_pos(t, cell_x(cell), cell_y(cell));
    if (appear) {
        scale_cb(t, 20);
        animate_scale(t, 20, CELL, 140, false);
    }
    return t;
}

static void anim_x(void *t, int32_t v) { lv_obj_set_x(t, (lv_coord_t)v); }
static void anim_y(void *t, int32_t v) { lv_obj_set_y(t, (lv_coord_t)v); }

static void slide(lv_obj_t *t, int to)
{
    lv_obj_set_user_data(t, (void *)(intptr_t)to);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_time(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_values(&a, lv_obj_get_x(t), cell_x(to));
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, lv_obj_get_y(t), cell_y(to));
    lv_anim_start(&a);
}

/* ---------- game logic ---------- */

/* k-th cell of line `line`, counting from the edge the tiles slide towards. */
static int line_cell(game_dir_t dir, int line, int k)
{
    switch (dir) {
    case GAME_DIR_LEFT:  return line * N + k;
    case GAME_DIR_RIGHT: return line * N + (N - 1 - k);
    case GAME_DIR_UP:    return k * N + line;
    default:             return (N - 1 - k) * N + line;   /* down */
    }
}

/* Puts a 2 (90%) or a 4 on a random empty cell; returns the cell or -1. */
static int spawn(void)
{
    int empty = 0;
    for (int i = 0; i < N * N; i++) empty += M.v[i] == 0;
    if (!empty) return -1;
    int k = (int)(game_random() % (uint32_t)empty);
    for (int i = 0; i < N * N; i++) {
        if (M.v[i] == 0 && k-- == 0) {
            M.v[i] = game_random() % 10 ? 2 : 4;
            return i;
        }
    }
    return -1;
}

static bool can_move(void)
{
    for (int i = 0; i < N * N; i++) {
        if (M.v[i] == 0) return true;
        if (i % N < N - 1 && M.v[i] == M.v[i + 1]) return true;
        if (i / N < N - 1 && M.v[i] == M.v[i + N]) return true;
    }
    return false;
}

static void new_game(void)
{
    memset(M.v, 0, sizeof(M.v));
    M.score = 0;
    M.won = false;
    M.started = true;
    spawn();
    spawn();
}

static void save_best(void)
{
    if (M.best > U.saved_best) {
        dash_set_int("2048_best", M.best);
        U.saved_best = M.best;
    }
}

static void update_scores(void)
{
    if (M.score > M.best) M.best = M.score;
    lv_label_set_text_fmt(U.score_val, "%d", M.score);
    lv_label_set_text_fmt(U.best_val, "%d", M.best);
}

/* Rebuilds every tile object from M (after New game or when opening). */
static void rebuild_tiles(void)
{
    for (int i = 0; i < N * N; i++) {
        if (U.tile[i]) lv_obj_del(U.tile[i]);
        U.tile[i] = M.v[i] ? tile_create(i, M.v[i], true) : NULL;
    }
    update_scores();
}

static void new_game_clicked(lv_event_t *e);
static void keep_going(lv_event_t *e);
static void do_move(game_dir_t dir);

static void finish_move(lv_timer_t *t)
{
    (void)t;
    U.finish = NULL;
    for (int i = 0; i < U.ndoomed; i++) lv_obj_del(U.doomed[i]);
    U.ndoomed = 0;

    bool reached_2048 = false;
    for (int i = 0; i < N * N; i++) {
        if (U.popped[i]) {
            tile_set_value(U.tile[i], M.v[i]);
            animate_scale(U.tile[i], CELL, CELL + 12, 70, true);
            if (M.v[i] == 2048 && !M.won) reached_2048 = true;
        }
    }
    if (U.spawned >= 0) {
        U.tile[U.spawned] = tile_create(U.spawned, M.v[U.spawned], true);
    }
    update_scores();
    U.busy = false;

    if (reached_2048) {
        M.won = true;
        save_best();
        lv_obj_t *row = game_dialog_open(U.parent, "You made 2048!");
        game_dialog_button(row, "Keep going", keep_going, NULL);
        game_dialog_button(row, LV_SYMBOL_REFRESH "  New game", new_game_clicked, NULL);
    } else if (!can_move()) {
        save_best();
        char title[48];
        snprintf(title, sizeof(title), "No more moves. Score: %d", M.score);
        lv_obj_t *row = game_dialog_open(U.parent, title);
        game_dialog_button(row, LV_SYMBOL_REFRESH "  New game", new_game_clicked, NULL);
    } else if (U.pending >= 0) {
        game_dir_t d = (game_dir_t)U.pending;
        U.pending = -1;
        do_move(d);
    }
}

static void do_move(game_dir_t dir)
{
    uint32_t nv[N * N] = { 0 };
    lv_obj_t *nt[N * N] = { 0 };
    bool merged[N * N] = { 0 };
    bool moved = false;
    U.ndoomed = 0;

    for (int line = 0; line < N; line++) {
        int out = 0;
        for (int k = 0; k < N; k++) {
            int src = line_cell(dir, line, k);
            if (!M.v[src]) continue;
            int prev = out > 0 ? line_cell(dir, line, out - 1) : -1;
            if (prev >= 0 && nv[prev] == M.v[src] && !merged[prev]) {
                nv[prev] *= 2;
                merged[prev] = true;
                M.score += (int)nv[prev];
                slide(U.tile[src], prev);
                U.doomed[U.ndoomed++] = U.tile[src];
                moved = true;
            } else {
                int dst = line_cell(dir, line, out++);
                nv[dst] = M.v[src];
                nt[dst] = U.tile[src];
                if (dst != src) {
                    slide(U.tile[src], dst);
                    moved = true;
                }
            }
        }
    }
    if (!moved) return;

    /* Tiles being merged into stay on top of the ones sliding onto them. */
    for (int i = 0; i < N * N; i++) {
        if (merged[i]) lv_obj_move_foreground(nt[i]);
    }

    memcpy(M.v, nv, sizeof(nv));
    memcpy(U.tile, nt, sizeof(nt));
    memcpy(U.popped, merged, sizeof(merged));
    U.spawned = spawn();
    U.busy = true;
    U.finish = lv_timer_create(finish_move, SLIDE_MS + 10, NULL);
    lv_timer_set_repeat_count(U.finish, 1);
}

static void swiped(game_dir_t dir)
{
    if (dir == GAME_DIR_TAP) return;
    if (U.busy) {
        U.pending = dir;              /* fast swipes: do it right after */
        return;
    }
    do_move(dir);
}

/* ---------- buttons ---------- */

static void new_game_clicked(lv_event_t *e)
{
    (void)e;
    game_dialog_close();
    if (U.busy) return;
    save_best();
    new_game();
    rebuild_tiles();
}

static void keep_going(lv_event_t *e)
{
    (void)e;
    game_dialog_close();
}

/* ---------- app entry points ---------- */

static lv_obj_t *score_box(lv_obj_t *parent, const char *name, int x, int w)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, 84);
    lv_obj_set_pos(box, x, BOARD_Y + 96);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_BOARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *n = lv_label_create(box);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_color(n, lv_color_hex(0xEEE4DA), 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *v = lv_label_create(box);
    lv_label_set_text(v, "0");
    lv_obj_set_style_text_font(v, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(v, lv_color_white(), 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -10);
    return v;
}

static void g2048_create(lv_obj_t *parent)
{
    s_game = dash_keep("game", sizeof(game_t));
    if (!s_game) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, "Not enough memory for the game.");
        lv_obj_center(l);
        return;
    }
    memset(&U, 0, sizeof(U));
    U.parent = parent;
    U.pending = -1;
    U.spawned = -1;
    U.saved_best = dash_get_int("2048_best", 0);
    if (!M.started) {
        M.best = U.saved_best;
        new_game();
    }

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_PAGE), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    U.board = lv_obj_create(parent);
    lv_obj_remove_style_all(U.board);
    lv_obj_set_size(U.board, BOARD_PX, BOARD_PX);
    lv_obj_set_pos(U.board, BOARD_X, BOARD_Y);
    lv_obj_set_style_radius(U.board, 10, 0);
    lv_obj_set_style_bg_color(U.board, lv_color_hex(C_BOARD), 0);
    lv_obj_set_style_bg_opa(U.board, LV_OPA_COVER, 0);
    lv_obj_clear_flag(U.board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < N * N; i++) {
        lv_obj_t *e = lv_obj_create(U.board);
        lv_obj_remove_style_all(e);
        lv_obj_set_size(e, CELL, CELL);
        lv_obj_set_pos(e, cell_x(i), cell_y(i));
        lv_obj_set_style_radius(e, 6, 0);
        lv_obj_set_style_bg_color(e, lv_color_hex(C_EMPTY), 0);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, 0);
        lv_obj_clear_flag(e, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_font(title, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(title, PANEL_X, BOARD_Y + 10);

    int bw = (PANEL_W - 12) / 2;
    U.score_val = score_box(parent, "SCORE", PANEL_X, bw);
    U.best_val = score_box(parent, "BEST", PANEL_X + bw + 12, bw);

    lv_obj_t *help = lv_label_create(parent);
    lv_label_set_text(help, "Swipe to slide the tiles. Two equal tiles "
                            "that touch merge into one. Reach 2048!");
    lv_label_set_long_mode(help, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(help, PANEL_W);
    lv_obj_set_style_text_color(help, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(help, PANEL_X, BOARD_Y + 200);

    /* Swipes work anywhere, so the layer goes under the button. */
    game_swipe_layer(parent, false, swiped);

    lv_obj_t *ng = game_button(parent, LV_SYMBOL_REFRESH "  New game", PANEL_W, 60,
                               new_game_clicked);
    lv_obj_set_pos(ng, PANEL_X, BOARD_Y + BOARD_PX - 60);
    lv_obj_set_style_bg_color(ng, lv_color_hex(0x8F7A66), 0);

    rebuild_tiles();
}

static void g2048_destroy(void)
{
    if (U.finish) {
        lv_timer_del(U.finish);   /* M is already up to date */
    }
    if (s_game) save_best();
    memset(&U, 0, sizeof(U));
}

int app_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    static const dash_app_def_t def = {
        .api_version = DASH_API_VERSION, .create = g2048_create, .destroy = g2048_destroy,
    };
    dash_app_register(&def);
    return 0;
}
