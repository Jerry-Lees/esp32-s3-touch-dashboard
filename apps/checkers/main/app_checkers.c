/*
 * Checkers app (American rules): play against the computer at four levels.
 *
 * Moving: tap a piece, then tap where it should land. For a multi-jump,
 * tap each landing square in turn; the piece follows along and the jumped
 * pieces fade until the move is complete.
 *
 * An SD card app with the same structure as apps/chess: the game is kept
 * in a dash_keep() block while the app is closed, and the computer thinks in
 * the game worker task.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dash_style.h"
#include "dashboard_app.h"
#include "checkers_engine.h"
#include "game_common.h"

extern const lv_img_dsc_t img_game_crown;          /* checkers_images.c */

#define HIST_MAX          600
#define RESULT_REPETITION 100     /* in addition to ck_status_t values */
#define MIN_THINK_US      500000

#define C_RED        0xC62828
#define C_RED_EDGE   0x7F1010
#define C_RED_RING   0xE57373
#define C_IVORY      0xEDE3CF
#define C_IVORY_EDGE 0x8C8270
#define C_IVORY_RING 0xFFFFFF

typedef struct {
    ck_pos_t  pos;                /* position before the move */
    ck_move_t move;
} hist_t;

/* ---- game state (a dash_keep block: kept while the app is closed) ---- */
typedef struct {
    bool      started;
    ck_pos_t  pos;
    hist_t    hist[HIST_MAX];
    int       hist_len;
    int8_t    human;              /* CK_RED or CK_WHITE */
    int       level;
    int       result;             /* ck_status_t or RESULT_REPETITION */
    ck_move_t last;
    bool      has_last;
    char      last_text[40];
} game_t;
static game_t *s_game;
#define G (*s_game)

/* ---- computer player ---- */
static struct {
    ck_pos_t  pos;
    int       level;
    uint32_t  gen;
    bool      ok;
    ck_move_t move;
} s_job;
static volatile bool s_abort;
static uint32_t      s_gen;
static bool          s_ai_running;
static int64_t       s_ai_started;

/* ---- UI ---- */
static struct {
    lv_obj_t *parent, *status, *detail, *spinner, *undo;
} ui;
static lv_timer_t *s_timer;
static ck_move_t   s_legal[CK_MAX_MOVES];
static int         s_nlegal;
static uint8_t     s_prefix[CK_MAX_PATH];   /* squares tapped so far */
static int         s_plen;
static int8_t      s_view[64];              /* board as drawn (mid-jump) */
static uint64_t    s_faded;                 /* pieces jumped mid-move */

/* ---------- helpers ---------- */

static void cancel_ai(void)
{
    s_gen++;
    s_abort = true;
}

static bool matches_prefix(const ck_move_t *m)
{
    if (m->len < s_plen) return false;
    for (int i = 0; i < s_plen; i++) {
        if (m->path[i] != s_prefix[i]) return false;
    }
    return true;
}

static void update_result(void)
{
    G.result = ck_status(&G.pos);
    if (G.result == CK_PLAYING) {
        uint32_t h = ck_hash(&G.pos);
        int seen = 0;
        for (int i = 0; i < G.hist_len; i++) {
            if (ck_hash(&G.hist[i].pos) == h) seen++;
        }
        if (seen >= 2) G.result = RESULT_REPETITION;
    }
}

static void refresh(void)
{
    if (!ui.status) return;

    bool my_turn = G.result == CK_PLAYING && G.pos.side == G.human && !s_ai_running;
    s_nlegal = my_turn ? ck_legal_moves(&G.pos, s_legal) : 0;
    bool must_jump = s_nlegal > 0 && s_legal[0].ncap > 0;

    /* What to draw: the real board, with a half-finished jump applied. */
    memcpy(s_view, G.pos.sq, sizeof(s_view));
    s_faded = 0;
    if (s_plen > 1) {
        int8_t pc = s_view[s_prefix[0]];
        s_view[s_prefix[0]] = 0;
        for (int i = 0; i + 1 < s_plen; i++) {
            s_faded |= GAME_BIT((s_prefix[i] + s_prefix[i + 1]) / 2);
        }
        s_view[s_prefix[s_plen - 1]] = pc;
    }

    uint64_t sel = 0, targets = 0, last = 0;
    for (int i = 0; i < s_plen; i++) sel |= GAME_BIT(s_prefix[i]);
    for (int i = 0; i < s_nlegal; i++) {
        if (s_plen == 0) {
            if (must_jump) sel |= GAME_BIT(s_legal[i].path[0]);   /* hint: these must jump */
        } else if (matches_prefix(&s_legal[i]) && s_legal[i].len > s_plen) {
            targets |= GAME_BIT(s_legal[i].path[s_plen]);
        }
    }
    if (G.has_last) {
        for (int i = 0; i < G.last.len; i++) last |= GAME_BIT(G.last.path[i]);
    }
    game_board_set_marks(sel, targets, last, 0);

    const char *st;
    switch (G.result) {
    case CK_LOSS:
        st = (G.pos.side == G.human) ? "You're out of moves.\nThe computer wins." : "You win!";
        break;
    case CK_DRAW:           st = "Draw\n(40 moves, no progress)"; break;
    case RESULT_REPETITION: st = "Draw\n(repetition)"; break;
    default:
        if (G.pos.side != G.human) st = "Thinking...";
        else if (s_plen > 1)       st = "Keep jumping!";
        else if (must_jump)        st = "Your move:\nyou must jump";
        else                       st = "Your move";
        break;
    }
    lv_label_set_text(ui.status, st);

    if (G.result == CK_PLAYING && G.pos.side != G.human) {
        lv_obj_clear_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui.spinner, LV_OBJ_FLAG_HIDDEN);
    }

    int mine = 0, theirs = 0;
    for (int s = 0; s < 64; s++) {
        int pc = G.pos.sq[s];
        if (pc && (pc > 0) == (G.human > 0)) mine++;
        else if (pc) theirs++;
    }
    lv_label_set_text_fmt(ui.detail, "You play %s\nPieces: you %d, computer %d\n%s",
                          G.human == CK_RED ? "Red" : "White", mine, theirs,
                          G.last_text);

    if (G.hist_len > 0) lv_obj_clear_state(ui.undo, LV_STATE_DISABLED);
    else lv_obj_add_state(ui.undo, LV_STATE_DISABLED);
}

static void play_move(const ck_move_t *m, bool by_computer)
{
    if (m->ncap > 1) {
        snprintf(G.last_text, sizeof(G.last_text), "%s %d pieces",
                 by_computer ? "Computer jumped" : "You jumped", m->ncap);
    } else if (m->ncap == 1) {
        snprintf(G.last_text, sizeof(G.last_text), "%s",
                 by_computer ? "Computer jumped a piece" : "You jumped a piece");
    } else {
        snprintf(G.last_text, sizeof(G.last_text), "%s",
                 by_computer ? "Computer moved" : "You moved");
    }

    if (G.hist_len == HIST_MAX) {
        memmove(G.hist, G.hist + 1, sizeof(hist_t) * (HIST_MAX - 1));
        G.hist_len--;
    }
    G.hist[G.hist_len].pos = G.pos;
    G.hist[G.hist_len].move = *m;
    G.hist_len++;

    ck_make(&G.pos, m);
    G.last = *m;
    G.has_last = true;
    s_plen = 0;
    update_result();
    refresh();
}

static void reset_game(int8_t human)
{
    cancel_ai();
    ck_new_game(&G.pos);
    G.hist_len = 0;
    G.human = human;
    G.result = CK_PLAYING;
    G.has_last = false;
    G.last_text[0] = 0;
    G.started = true;
    s_plen = 0;
}

/* ---------- computer player ---------- */

static void ai_job(void *arg)
{
    /* Runs in the worker task: no LVGL here. */
    (void)arg;
    s_job.ok = ck_search(&s_job.pos, s_job.level, &s_abort, &s_job.move);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (s_ai_running) {
        if (game_worker_busy()) return;
        if (s_job.gen != s_gen) {
            s_ai_running = false;
        } else if (game_time_us() - s_ai_started < MIN_THINK_US) {
            return;
        } else {
            s_ai_running = false;
            if (s_job.ok) play_move(&s_job.move, true);
            return;
        }
    }
    if (G.result == CK_PLAYING && G.pos.side != G.human && !game_worker_busy()) {
        s_job.pos = G.pos;
        s_job.level = G.level;
        s_job.gen = s_gen;
        s_job.ok = false;
        s_abort = false;
        if (game_worker_start(ai_job, NULL)) {
            s_ai_running = true;
            s_ai_started = game_time_us();
            refresh();
        }
    }
}

/* ---------- input ---------- */

static void board_tapped(int row, int col)
{
    if (G.result != CK_PLAYING || G.pos.side != G.human || s_ai_running) return;
    int sq = row * 8 + col;

    /* Continue the selected move if this is a valid next square. */
    if (s_plen > 0) {
        for (int i = 0; i < s_nlegal; i++) {
            const ck_move_t *m = &s_legal[i];
            if (matches_prefix(m) && m->len > s_plen && m->path[s_plen] == sq) {
                s_prefix[s_plen++] = (uint8_t)sq;
                for (int j = 0; j < s_nlegal; j++) {
                    if (matches_prefix(&s_legal[j]) && s_legal[j].len == s_plen) {
                        ck_move_t done = s_legal[j];
                        play_move(&done, false);
                        return;
                    }
                }
                refresh();
                return;
            }
        }
    }

    /* Otherwise (re)select a piece that can move, or clear the selection. */
    bool was_selected = s_plen == 1 && s_prefix[0] == sq;
    s_plen = 0;
    if (!was_selected) {
        for (int i = 0; i < s_nlegal; i++) {
            if (s_legal[i].path[0] == sq) {
                s_prefix[0] = (uint8_t)sq;
                s_plen = 1;
                break;
            }
        }
    }
    refresh();
}

static void draw_square(lv_draw_ctx_t *ctx, const lv_area_t *area, int row, int col)
{
    int sq = row * 8 + col;
    int pc = s_view[sq];
    if (!pc) return;

    lv_opa_t opa = (s_faded & GAME_BIT(sq)) ? LV_OPA_40 : LV_OPA_COVER;
    bool red = pc > 0;

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(red ? C_RED : C_IVORY);
    d.bg_opa = opa;
    d.border_color = lv_color_hex(red ? C_RED_EDGE : C_IVORY_EDGE);
    d.border_width = 3;
    d.border_opa = opa;
    lv_area_t a = { area->x1 + 5, area->y1 + 5, area->x2 - 5, area->y2 - 5 };
    lv_draw_rect(ctx, &d, &a);

    /* Inner ring for a bit of depth. */
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = lv_color_hex(red ? C_RED_RING : C_IVORY_RING);
    d.border_width = 2;
    d.border_opa = opa;
    lv_area_t r = { area->x1 + 12, area->y1 + 12, area->x2 - 12, area->y2 - 12 };
    lv_draw_rect(ctx, &d, &r);

    if (abs(pc) == 2 && opa == LV_OPA_COVER) {
        game_board_draw_img(ctx, area, &img_game_crown);
    }
}

static void new_game_choice(lv_event_t *e)
{
    int choice = (int)(intptr_t)lv_event_get_user_data(e);
    game_dialog_close();
    if (choice == 0) return;
    reset_game((int8_t)choice);
    game_board_set_flipped(G.human == CK_WHITE);
    refresh();
}

static void new_game_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *row = game_dialog_open(ui.parent, "New game: play as");
    game_dialog_button(row, "Red (first)", new_game_choice, (void *)(intptr_t)CK_RED);
    game_dialog_button(row, "White", new_game_choice, (void *)(intptr_t)CK_WHITE);
    game_dialog_button(row, "Cancel", new_game_choice, (void *)(intptr_t)0);
}

static void undo_clicked(lv_event_t *e)
{
    (void)e;
    if (G.hist_len == 0) return;
    cancel_ai();
    do {
        G.hist_len--;
        G.pos = G.hist[G.hist_len].pos;
    } while (G.hist_len > 0 && G.pos.side != G.human);
    G.has_last = G.hist_len > 0;
    if (G.has_last) G.last = G.hist[G.hist_len - 1].move;
    snprintf(G.last_text, sizeof(G.last_text), "Move taken back");
    s_plen = 0;
    update_result();
    refresh();
}

static void level_changed(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id < GAME_LEVELS) {
        G.level = id;
        dash_set_int("checkers_lvl", id);
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

static void checkers_create(lv_obj_t *parent)
{
    s_game = dash_keep("game", sizeof(game_t));
    if (!s_game) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, "Not enough memory for the game.");
        lv_obj_center(l);
        return;
    }
    if (!G.started) {
        G.level = dash_get_int("checkers_lvl", 1);
        reset_game(CK_RED);
    }

    ui.parent = parent;
    game_board_cfg_t cfg = {
        .light = 0xEAD8B1, .dark = 0x5B8746, .coords = false,
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

    game_board_set_flipped(G.human == CK_WHITE);
    refresh();
    s_timer = lv_timer_create(tick, 100, NULL);
}

static void checkers_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    cancel_ai();
    memset(&ui, 0, sizeof(ui));
    s_plen = 0;
}

int app_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    static const dash_app_def_t def = {
        .api_version = DASH_API_VERSION, .create = checkers_create, .destroy = checkers_destroy,
    };
    dash_app_register(&def);
    return 0;
}
