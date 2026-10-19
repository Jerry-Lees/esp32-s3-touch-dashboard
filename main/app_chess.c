/*
 * Chess app: play against the computer at four difficulty levels.
 *
 * The game lives in static memory, so leaving the app and coming back
 * resumes it (until the device restarts). The computer's search runs in
 * the game worker task (game_common.c); the lv_timer `tick` starts it and
 * picks up its answer. s_gen invalidates a search whose position is no
 * longer current (undo, new game, leaving the app).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "lvgl.h"
#include "app_framework.h"
#include "chess_engine.h"
#include "game_common.h"
#include "settings.h"

#define HIST_MAX          600
#define RESULT_REPETITION 100     /* in addition to chess_status_t values */
#define MIN_THINK_US      500000  /* show "Thinking..." at least this long */

typedef struct {
    chess_pos_t  pos;             /* position before the move */
    chess_move_t move;
} hist_t;

/* ---- game state (kept while the app is closed) ---- */
static struct {
    bool         started;
    chess_pos_t  pos;
    hist_t      *hist;
    int          hist_len;
    int8_t       human;           /* CH_WHITE or CH_BLACK */
    int          level;
    int          result;          /* chess_status_t or RESULT_REPETITION */
    chess_move_t last;
    char         last_text[40];
} G;

/* ---- computer player ---- */
static struct {
    chess_pos_t  pos;
    int          level;
    uint32_t     gen;
    chess_move_t result;
} s_job;
static volatile bool s_abort;
static uint32_t      s_gen;
static bool          s_ai_running;
static int64_t       s_ai_started;

/* ---- UI ---- */
static struct {
    lv_obj_t *parent, *status, *detail, *spinner, *undo;
} ui;
static lv_timer_t  *s_timer;
static int          s_sel = -1;
static chess_move_t s_legal[CH_MAX_MOVES];
static int          s_nlegal;
static int          s_promo_from, s_promo_to;

/* ---------- helpers ---------- */

static const lv_img_dsc_t *piece_img(int pc)
{
    static const lv_img_dsc_t *const W[7] = {
        NULL, &img_chess_w_pawn, &img_chess_w_knight, &img_chess_w_bishop,
        &img_chess_w_rook, &img_chess_w_queen, &img_chess_w_king };
    static const lv_img_dsc_t *const B[7] = {
        NULL, &img_chess_b_pawn, &img_chess_b_knight, &img_chess_b_bishop,
        &img_chess_b_rook, &img_chess_b_queen, &img_chess_b_king };
    return pc > 0 ? W[pc] : B[-pc];
}

static void cancel_ai(void)
{
    s_gen++;          /* any running search's answer will be ignored */
    s_abort = true;   /* and it stops early */
}

static void update_result(void)
{
    G.result = chess_status(&G.pos);
    if (G.result == CH_PLAYING) {
        uint32_t h = chess_hash(&G.pos);
        int seen = 0;
        for (int i = 0; i < G.hist_len; i++) {
            if (chess_hash(&G.hist[i].pos) == h) seen++;
        }
        if (seen >= 2) G.result = RESULT_REPETITION;
    }
}

static void refresh(void)
{
    if (!ui.status) return;

    bool my_turn = G.result == CH_PLAYING && G.pos.side == G.human && !s_ai_running;
    s_nlegal = my_turn ? chess_legal_moves(&G.pos, s_legal) : 0;

    uint64_t sel = 0, targets = 0, last = 0, alert = 0;
    if (s_sel >= 0) {
        sel = GAME_BIT(s_sel);
        for (int i = 0; i < s_nlegal; i++) {
            if (CM_FROM(s_legal[i]) == s_sel) targets |= GAME_BIT(CM_TO(s_legal[i]));
        }
    }
    if (G.last) last = GAME_BIT(CM_FROM(G.last)) | GAME_BIT(CM_TO(G.last));
    bool check = chess_in_check(&G.pos);
    if (check) alert = GAME_BIT(G.pos.king[G.pos.side == CH_WHITE ? 0 : 1]);
    game_board_set_marks(sel, targets, last, alert);

    const char *st;
    switch (G.result) {
    case CH_CHECKMATE:
        st = (G.pos.side == G.human) ? "Checkmate.\nThe computer wins." : "Checkmate!\nYou win!";
        break;
    case CH_STALEMATE:      st = "Stalemate.\nIt's a draw."; break;
    case CH_DRAW_50:        st = "Draw\n(50-move rule)"; break;
    case CH_DRAW_MATERIAL:  st = "Draw\n(not enough pieces)"; break;
    case RESULT_REPETITION: st = "Draw\n(repetition)"; break;
    default:
        if (G.pos.side != G.human) st = "Thinking...";
        else st = check ? "Check!\nYour move" : "Your move";
        break;
    }
    lv_label_set_text(ui.status, st);

    if (G.result == CH_PLAYING && G.pos.side != G.human) {
        lv_obj_clear_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text_fmt(ui.detail, "You play %s\n%s",
                          G.human == CH_WHITE ? "White" : "Black", G.last_text);

    if (G.hist_len > 0) lv_obj_clear_state(ui.undo, LV_STATE_DISABLED);
    else lv_obj_add_state(ui.undo, LV_STATE_DISABLED);
}

static void play_move(chess_move_t m, bool by_computer)
{
    char san[16];
    chess_move_to_san(&G.pos, m, san, sizeof(san));
    snprintf(G.last_text, sizeof(G.last_text), "%s %s",
             by_computer ? "Computer played" : "You played", san);

    if (G.hist_len == HIST_MAX) {
        memmove(G.hist, G.hist + 1, sizeof(hist_t) * (HIST_MAX - 1));
        G.hist_len--;
    }
    G.hist[G.hist_len].pos = G.pos;
    G.hist[G.hist_len].move = m;
    G.hist_len++;

    chess_make(&G.pos, m);
    G.last = m;
    s_sel = -1;
    update_result();
    refresh();
}

static void reset_game(int8_t human)
{
    cancel_ai();
    chess_new_game(&G.pos);
    G.hist_len = 0;
    G.human = human;
    G.result = CH_PLAYING;
    G.last = 0;
    G.last_text[0] = 0;
    G.started = true;
    s_sel = -1;
}

/* ---------- computer player ---------- */

static void ai_job(void *arg)
{
    /* Runs in the worker task: no LVGL here. */
    (void)arg;
    s_job.result = chess_search(&s_job.pos, s_job.level, &s_abort);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (s_ai_running) {
        if (game_worker_busy()) return;
        if (s_job.gen != s_gen) {
            s_ai_running = false;               /* stale answer: drop it */
        } else if (game_time_us() - s_ai_started < MIN_THINK_US) {
            return;
        } else {
            s_ai_running = false;
            if (s_job.result) play_move(s_job.result, true);
            return;
        }
    }
    if (G.result == CH_PLAYING && G.pos.side != G.human && !game_worker_busy()) {
        s_job.pos = G.pos;
        s_job.level = G.level;
        s_job.gen = s_gen;
        s_job.result = 0;
        s_abort = false;
        if (game_worker_start(ai_job, NULL)) {
            s_ai_running = true;
            s_ai_started = game_time_us();
            refresh();
        }
    }
}

/* ---------- input ---------- */

static void promo_clicked(lv_event_t *e)
{
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    game_dialog_close();
    for (int i = 0; i < s_nlegal; i++) {
        chess_move_t m = s_legal[i];
        if (CM_FROM(m) == s_promo_from && CM_TO(m) == s_promo_to && CM_PROMO(m) == type) {
            play_move(m, false);
            return;
        }
    }
}

static void ask_promotion(int from, int to)
{
    static const int types[4] = { CH_QUEEN, CH_ROOK, CH_BISHOP, CH_KNIGHT };
    s_promo_from = from;
    s_promo_to = to;
    lv_obj_t *row = game_dialog_open(ui.parent, "Promote to");
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = game_dialog_button(row, NULL, promo_clicked, (void *)(intptr_t)types[i]);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x8D99A6), 0);
        lv_obj_t *img = lv_img_create(b);
        lv_img_set_src(img, piece_img(G.human * types[i]));
        lv_obj_center(img);
    }
}

static void board_tapped(int row, int col)
{
    if (G.result != CH_PLAYING || G.pos.side != G.human || s_ai_running) return;
    int sq = row * 8 + col;

    if (s_sel >= 0) {
        int found = 0;
        chess_move_t m = 0;
        for (int i = 0; i < s_nlegal; i++) {
            if (CM_FROM(s_legal[i]) == s_sel && CM_TO(s_legal[i]) == sq) {
                found++;
                m = s_legal[i];
            }
        }
        if (found == 1) {
            play_move(m, false);
            return;
        }
        if (found > 1) {           /* the same squares with 4 promotion choices */
            ask_promotion(s_sel, sq);
            return;
        }
    }

    int pc = G.pos.sq[sq];
    s_sel = (pc != 0 && (pc > 0) == (G.human > 0) && sq != s_sel) ? sq : -1;
    refresh();
}

static void draw_square(lv_draw_ctx_t *ctx, const lv_area_t *area, int row, int col)
{
    int pc = G.pos.sq[row * 8 + col];
    if (pc) game_board_draw_img(ctx, area, piece_img(pc));
}

static void new_game_choice(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    game_dialog_close();
    if (choice == 0) return;
    reset_game((int8_t)choice);
    game_board_set_flipped(G.human == CH_BLACK);
    refresh();
}

static void new_game_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *row = game_dialog_open(ui.parent, "New game: play as");
    game_dialog_button(row, "White", new_game_choice, (void *)(intptr_t)CH_WHITE);
    game_dialog_button(row, "Black", new_game_choice, (void *)(intptr_t)CH_BLACK);
    game_dialog_button(row, "Cancel", new_game_choice, (void *)(intptr_t)0);
}

static void undo_clicked(lv_event_t *e)
{
    (void)e;
    if (G.hist_len == 0) return;
    cancel_ai();
    /* Take back moves until it's the player's turn again. */
    do {
        G.hist_len--;
        G.pos = G.hist[G.hist_len].pos;
    } while (G.hist_len > 0 && G.pos.side != G.human);
    G.last = G.hist_len ? G.hist[G.hist_len - 1].move : 0;
    snprintf(G.last_text, sizeof(G.last_text), "Move taken back");
    s_sel = -1;
    update_result();
    refresh();
}

static void level_changed(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id < GAME_LEVELS) {
        G.level = id;
        settings_set_u8("chess_lvl", id);
    }
}

/* ---------- app entry points ---------- */

static lv_obj_t *panel_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                             lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    lv_obj_set_pos(l, GAME_PANEL_X, y);
    return l;
}

static void chess_create(lv_obj_t *parent)
{
    if (!G.hist) {
        G.hist = heap_caps_malloc(sizeof(hist_t) * HIST_MAX, MALLOC_CAP_SPIRAM);
        if (!G.hist) G.hist = malloc(sizeof(hist_t) * HIST_MAX);
    }
    if (!G.started) {
        G.level = settings_get_u8("chess_lvl", 1);
        reset_game(CH_WHITE);
    }

    ui.parent = parent;
    game_board_cfg_t cfg = {
        .light = 0xF0D9B5, .dark = 0xB58863, .coords = true,
        .draw = draw_square, .tap = board_tapped,
    };
    lv_obj_t *board = game_board_create(parent, &cfg);
    lv_obj_set_pos(board, 8, 8);

    ui.status = panel_label(parent, APP_FONT_LARGE, APP_COLOR_TEXT, 8, GAME_PANEL_W - 52);
    ui.spinner = lv_spinner_create(parent, 1000, 60);
    lv_obj_set_size(ui.spinner, 40, 40);
    lv_obj_set_pos(ui.spinner, GAME_PANEL_X + GAME_PANEL_W - 44, 8);
    lv_obj_set_style_arc_width(ui.spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.spinner, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.spinner, lv_color_hex(0x2A333C), LV_PART_MAIN);

    ui.detail = panel_label(parent, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, 88, GAME_PANEL_W);

    lv_obj_t *dl = panel_label(parent, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, 186, GAME_PANEL_W);
    lv_label_set_text(dl, "Computer difficulty");
    lv_obj_t *levels = game_level_selector(parent, G.level, level_changed);
    lv_obj_set_pos(levels, GAME_PANEL_X, 210);

    lv_coord_t bw = (GAME_PANEL_W - 12) / 2;
    lv_obj_t *ng = game_button(parent, LV_SYMBOL_REFRESH "  New game", bw, 60, new_game_clicked);
    lv_obj_set_pos(ng, GAME_PANEL_X, 336);
    ui.undo = game_button(parent, LV_SYMBOL_LEFT "  Undo", bw, 60, undo_clicked);
    lv_obj_set_pos(ui.undo, GAME_PANEL_X + bw + 12, 336);

    game_board_set_flipped(G.human == CH_BLACK);
    refresh();
    s_timer = lv_timer_create(tick, 100, NULL);
}

static void chess_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    /* Stop thinking; the computer starts again when the app reopens. */
    cancel_ai();
    memset(&ui, 0, sizeof(ui));
    s_sel = -1;
}

static void chess_tile_icon(lv_obj_t *holder, int size)
{
    lv_obj_t *img = lv_img_create(holder);
    lv_img_set_src(img, &img_chess_w_knight);
    lv_img_set_zoom(img, (uint16_t)(256 * size / img_chess_w_knight.header.w));
    lv_obj_center(img);
}

const app_t chess_app = {
    .name      = "Chess",
    .symbol    = LV_SYMBOL_SHUFFLE,
    .color_hex = 0x6D4C41,
    .create    = chess_create,
    .destroy   = chess_destroy,
    .draw_icon = chess_tile_icon,
};
