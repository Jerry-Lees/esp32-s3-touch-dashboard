/*
 * Snake: swipe to steer, eat apples, don't hit the walls or yourself.
 *
 * The board is one object with a custom draw callback that paints the
 * checkerboard, the snake and the apple. Each step only invalidates the few
 * cells that changed, so redrawing stays cheap. One lv_timer moves the
 * snake; its period shrinks as the score rises (the snake speeds up).
 * The best score is saved with dash_set_int.
 *
 * An SD card app: tools/build_app.sh apps/snake
 */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "dash_style.h"
#include "dashboard_app.h"
#include "game_common.h"

#define COLS     27
#define ROWS     13
#define CELLS    (COLS * ROWS)
#define CELL     28
#define BOARD_X  22
#define BOARD_Y  44

#define C_BG       0x578A34
#define C_LIGHT    0xAAD751
#define C_DARK     0xA2D149
#define C_SNAKE    0x4A75E6
#define C_HEAD     0x3A5FCD
#define C_APPLE    0xE7471D
#define C_LEAF     0x2E7D32

enum { ST_READY, ST_PLAYING, ST_OVER };

static const int8_t DX[4] = { 0, 1, 0, -1 };     /* indexed by game_dir_t */
static const int8_t DY[4] = { -1, 0, 1, 0 };

static struct {
    int        state;
    uint16_t   body[CELLS];       /* ring buffer of cell indices, head first */
    int        head, len;         /* body[head] is the head */
    bool       occ[CELLS];
    int        dir;               /* direction of the last step */
    int        queue[2], nqueue;  /* swipes waiting for the next steps */
    int        apple;
    int        score, best;
    bool       won;

    lv_obj_t  *board, *score_lbl, *best_lbl, *hint, *panel, *panel_text;
    lv_timer_t *timer;
} S;

static int seg(int i) { return S.body[(S.head + i) % CELLS]; }   /* i = 0 is the head */

static int step_ms(void)
{
    int ms = 150 - 3 * S.score;
    return ms < 70 ? 70 : ms;
}

/* ---------- drawing ---------- */

static void cell_area(int cell, lv_area_t *a)
{
    lv_area_t b;
    lv_obj_get_coords(S.board, &b);
    a->x1 = b.x1 + (cell % COLS) * CELL;
    a->y1 = b.y1 + (cell / COLS) * CELL;
    a->x2 = a->x1 + CELL - 1;
    a->y2 = a->y1 + CELL - 1;
}

static void inval_cell(int cell)
{
    lv_area_t a;
    cell_area(cell, &a);
    lv_obj_invalidate_area(S.board, &a);
}

static void fill(lv_draw_ctx_t *ctx, const lv_area_t *a, uint32_t color, int radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_draw_rect(ctx, &d, a);
}

static void draw_eyes(lv_draw_ctx_t *ctx, const lv_area_t *a)
{
    int cx = (a->x1 + a->x2) / 2, cy = (a->y1 + a->y2) / 2;
    int fx = DX[S.dir], fy = DY[S.dir];       /* forward */
    int sx = -fy, sy = fx;                     /* sideways */
    for (int k = -1; k <= 1; k += 2) {
        int ex = cx + fx * 4 + sx * 6 * k, ey = cy + fy * 4 + sy * 6 * k;
        lv_area_t w = { ex - 4, ey - 4, ex + 4, ey + 4 };
        fill(ctx, &w, 0xFFFFFF, LV_RADIUS_CIRCLE);
        lv_area_t p = { ex + fx * 2 - 2, ey + fy * 2 - 2, ex + fx * 2 + 2, ey + fy * 2 + 2 };
        fill(ctx, &p, 0x1B1B1B, LV_RADIUS_CIRCLE);
    }
}

static void board_draw(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t b;
    lv_obj_get_coords(S.board, &b);

    /* Checkerboard: only the cells inside the area being redrawn. */
    const lv_area_t *clip = ctx->clip_area;
    int c0 = LV_MAX(0, (clip->x1 - b.x1) / CELL), c1 = LV_MIN(COLS - 1, (clip->x2 - b.x1) / CELL);
    int r0 = LV_MAX(0, (clip->y1 - b.y1) / CELL), r1 = LV_MIN(ROWS - 1, (clip->y2 - b.y1) / CELL);
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            lv_area_t a;
            cell_area(r * COLS + c, &a);
            fill(ctx, &a, (r + c) & 1 ? C_DARK : C_LIGHT, 0);
        }
    }

    if (S.apple >= 0) {
        lv_area_t a;
        cell_area(S.apple, &a);
        lv_area_t ap = { a.x1 + 4, a.y1 + 5, a.x2 - 4, a.y2 - 3 };
        fill(ctx, &ap, C_APPLE, LV_RADIUS_CIRCLE);
        lv_area_t leaf = { a.x1 + 14, a.y1 + 1, a.x1 + 20, a.y1 + 6 };
        fill(ctx, &leaf, C_LEAF, 3);
    }

    /* Snake: a rounded square per segment plus a bridge to the next one,
     * so the body looks like one continuous tube. */
    for (int i = S.len - 1; i >= 0; i--) {
        lv_area_t a;
        cell_area(seg(i), &a);
        if (i + 1 < S.len) {
            /* Bridge from this segment's centre to the next one's. */
            lv_area_t n;
            cell_area(seg(i + 1), &n);
            lv_area_t br;
            if (a.y1 == n.y1) {
                br = (lv_area_t){ LV_MIN(a.x1, n.x1) + CELL / 2, a.y1 + 3,
                                  LV_MAX(a.x1, n.x1) + CELL / 2, a.y2 - 3 };
            } else {
                br = (lv_area_t){ a.x1 + 3, LV_MIN(a.y1, n.y1) + CELL / 2,
                                  a.x2 - 3, LV_MAX(a.y1, n.y1) + CELL / 2 };
            }
            fill(ctx, &br, C_SNAKE, 0);
        }
        lv_area_t s = { a.x1 + 3, a.y1 + 3, a.x2 - 3, a.y2 - 3 };
        fill(ctx, &s, i == 0 ? C_HEAD : C_SNAKE, 8);
    }
    if (S.len > 0) {
        lv_area_t a;
        cell_area(seg(0), &a);
        draw_eyes(ctx, &a);
    }
}

/* ---------- game logic ---------- */

static void update_labels(void)
{
    lv_label_set_text_fmt(S.score_lbl, LV_SYMBOL_PLAY "  Score: %d", S.score);
    lv_label_set_text_fmt(S.best_lbl, "Best: %d", S.best);
}

static void place_apple(void)
{
    int free_cells = CELLS - S.len;
    if (free_cells <= 0) {
        S.apple = -1;
        return;
    }
    int k = (int)(game_random() % (uint32_t)free_cells);
    for (int c = 0; c < CELLS; c++) {
        if (!S.occ[c] && k-- == 0) {
            S.apple = c;
            inval_cell(c);
            return;
        }
    }
}

static void go_ready(void)
{
    S.state = ST_READY;
    memset(S.occ, 0, sizeof(S.occ));
    S.len = 4;
    S.head = 0;
    S.dir = GAME_DIR_RIGHT;
    S.nqueue = 0;
    S.score = 0;
    S.won = false;
    int row = ROWS / 2;
    for (int i = 0; i < S.len; i++) {
        int c = row * COLS + 7 - i;
        S.body[i] = (uint16_t)c;
        S.occ[c] = true;
    }
    S.apple = -1;
    place_apple();
    update_labels();
    lv_obj_invalidate(S.board);
    lv_obj_clear_flag(S.hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(S.panel, LV_OBJ_FLAG_HIDDEN);
    lv_timer_set_period(S.timer, step_ms());
}

static void game_over(void)
{
    S.state = ST_OVER;
    if (S.score > S.best) {
        S.best = S.score;
        dash_set_int("snake_best", S.best);
        update_labels();
    }
    lv_label_set_text_fmt(S.panel_text, "%s\nScore: %d\nBest: %d\n\nTap to play again",
                          S.won ? "You filled the board!" : "Game over",
                          S.score, S.best);
    lv_obj_clear_flag(S.panel, LV_OBJ_FLAG_HIDDEN);
    inval_cell(seg(0));
}

static void step(lv_timer_t *t)
{
    (void)t;
    if (S.state != ST_PLAYING) return;

    if (S.nqueue > 0) {
        S.dir = S.queue[0];
        S.queue[0] = S.queue[1];
        S.nqueue--;
    }

    int h = seg(0);
    int x = h % COLS + DX[S.dir], y = h / COLS + DY[S.dir];
    if (x < 0 || x >= COLS || y < 0 || y >= ROWS) {
        game_over();
        return;
    }
    int nh = y * COLS + x;
    bool eat = nh == S.apple;
    int tail = seg(S.len - 1);

    if (!eat) {                       /* tail moves out of the way first */
        S.occ[tail] = false;
        S.len--;
        inval_cell(tail);
        inval_cell(seg(S.len - 1));
    }
    if (S.occ[nh]) {
        if (!eat) {                   /* put the tail back for the final picture */
            S.occ[tail] = true;
            S.len++;
        }
        game_over();
        return;
    }

    S.head = (S.head + CELLS - 1) % CELLS;
    S.body[S.head] = (uint16_t)nh;
    S.occ[nh] = true;
    S.len++;
    inval_cell(h);
    inval_cell(nh);

    if (eat) {
        S.score++;
        update_labels();
        lv_timer_set_period(S.timer, step_ms());
        place_apple();
        if (S.apple < 0) {
            S.won = true;
            game_over();
        }
    }
}

static void swiped(game_dir_t dir)
{
    switch (S.state) {
    case ST_READY:
        S.state = ST_PLAYING;
        lv_obj_add_flag(S.hint, LV_OBJ_FLAG_HIDDEN);
        if (dir != GAME_DIR_TAP && dir != GAME_DIR_LEFT) {
            S.dir = dir;              /* can't start by reversing into the body */
        }
        lv_timer_reset(S.timer);
        return;
    case ST_OVER:
        if (dir == GAME_DIR_TAP) go_ready();
        return;
    default:
        break;
    }
    if (dir == GAME_DIR_TAP) return;

    /* Compare with the last queued turn (or the current direction): ignore
     * the same direction and straight reversals. */
    int last = S.nqueue ? S.queue[S.nqueue - 1] : S.dir;
    if (dir == last || dir == (last + 2) % 4) return;
    if (S.nqueue < 2) S.queue[S.nqueue++] = dir;
}

/* ---------- app entry points ---------- */

static lv_obj_t *hud_label(lv_obj_t *parent, lv_align_t align, int x)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_align(l, align, x, 10);
    return l;
}

static void snake_create(lv_obj_t *parent)
{
    memset(&S, 0, sizeof(S));
    S.best = dash_get_int("snake_best", 0);

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    S.score_lbl = hud_label(parent, LV_ALIGN_TOP_LEFT, BOARD_X);
    S.best_lbl = hud_label(parent, LV_ALIGN_TOP_RIGHT, -BOARD_X);

    S.board = lv_obj_create(parent);
    lv_obj_remove_style_all(S.board);
    lv_obj_set_size(S.board, COLS * CELL, ROWS * CELL);
    lv_obj_set_pos(S.board, BOARD_X, BOARD_Y);
    lv_obj_clear_flag(S.board, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(S.board, board_draw, LV_EVENT_DRAW_MAIN, NULL);

    S.hint = lv_label_create(parent);
    lv_label_set_text(S.hint, "Swipe to steer\nSwipe or tap to start");
    lv_obj_set_style_text_font(S.hint, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(S.hint, lv_color_white(), 0);
    lv_obj_set_style_text_align(S.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(S.hint, lv_color_hex(0x1E252C), 0);
    lv_obj_set_style_bg_opa(S.hint, LV_OPA_60, 0);
    lv_obj_set_style_radius(S.hint, 14, 0);
    lv_obj_set_style_pad_hor(S.hint, 24, 0);
    lv_obj_set_style_pad_ver(S.hint, 12, 0);
    lv_obj_align(S.hint, LV_ALIGN_CENTER, 0, 70);

    S.panel = lv_obj_create(parent);
    lv_obj_set_size(S.panel, 340, LV_SIZE_CONTENT);
    lv_obj_align(S.panel, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_radius(S.panel, 18, 0);
    lv_obj_set_style_bg_color(S.panel, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(S.panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(S.panel, 0, 0);
    lv_obj_set_style_pad_all(S.panel, 20, 0);
    lv_obj_clear_flag(S.panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    S.panel_text = lv_label_create(S.panel);
    lv_obj_set_width(S.panel_text, LV_PCT(100));
    lv_obj_set_style_text_font(S.panel_text, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(S.panel_text, lv_color_white(), 0);
    lv_obj_set_style_text_align(S.panel_text, LV_TEXT_ALIGN_CENTER, 0);

    game_swipe_layer(parent, true, swiped);

    S.timer = lv_timer_create(step, step_ms(), NULL);
    go_ready();
}

static void snake_destroy(void)
{
    if (S.timer) {
        lv_timer_del(S.timer);
    }
    memset(&S, 0, sizeof(S));
}

int app_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    static const dash_app_def_t def = {
        .api_version = DASH_API_VERSION, .create = snake_create, .destroy = snake_destroy,
    };
    dash_app_register(&def);
    return 0;
}
