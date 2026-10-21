/*
 * Flappy: tap to flap, fly through the gaps between the pipes.
 *
 * One lv_timer runs the game: it moves the bird and pipes using the real
 * time since the last frame (so speed doesn't depend on frame rate) and
 * repositions a fixed set of LVGL objects. Pipes that leave the screen on
 * the left are reused on the right with a new gap. The ground stripes are
 * drawn by a custom draw callback so scrolling them is cheap.
 *
 * The game gets slightly harder as the score rises: pipes move faster and
 * gaps get narrower, up to a limit. The best score is saved with dash_set_int.
 *
 * An SD card app: tools/build_app.sh apps/flappy
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "dash_style.h"
#include "dashboard_app.h"
#include "game_sys.h"

extern const lv_img_dsc_t img_flappy_bird_0, img_flappy_bird_1;

#define W            800
#define PLAY_H       360          /* sky; the ground fills the rest (56 px) */
#define BIRD_X       200          /* bird's centre x */
#define BIRD_R       13           /* hit-circle radius (sprite is 48 x 36) */
#define PIPE_W       78
#define CAP_W        92
#define CAP_H        28
#define PIPE_SPACING 280
#define N_PIPES      4

#define GRAVITY      1400.0f      /* px/s^2 */
#define FLAP_VY      (-430.0f)    /* px/s, upwards */
#define MAX_VY       650.0f

#define C_SKY        0x4EC0CA
#define C_PIPE_LT    0x9BE35A
#define C_PIPE_DK    0x4E9A22
#define C_PIPE_EDGE  0x2F5E14
#define C_GROUND     0xDED895
#define C_GRASS      0x73BF2E
#define C_GRASS_DK   0x5A9E22

enum { ST_READY, ST_PLAYING, ST_FALLING, ST_OVER };

typedef struct {
    float     x;                  /* left edge of the pipe body */
    int       gap_y, gap_h;       /* top of the gap and its height */
    bool      passed;
    lv_obj_t *top, *top_cap, *bot, *bot_cap;
} pipe_t;

static struct {
    int        state;
    float      y, vy;             /* bird centre y and speed */
    float      t;                 /* seconds in the current state */
    float      ground_off;
    int        score, best;
    bool       new_best;
    int64_t    last_us;
    pipe_t     pipes[N_PIPES];

    lv_obj_t  *bird, *ground, *score_lbl, *score_shadow, *hint, *panel, *panel_text;
    lv_timer_t *timer;
} F;

/* ---------- difficulty ---------- */

static float pipe_speed(void) { return fminf(250.0f, 170.0f + 3.0f * F.score); }
static int   gap_height(void) { return F.score >= 18 ? 124 : 160 - 2 * F.score; }

/* ---------- pipes ---------- */

static lv_obj_t *pipe_part(lv_obj_t *parent, bool cap)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_PIPE_LT), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(C_PIPE_DK), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_PIPE_EDGE), 0);
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_set_style_radius(o, cap ? 5 : 0, 0);
    lv_obj_set_size(o, cap ? CAP_W : PIPE_W, cap ? CAP_H : 10);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* New random gap, not too far from the previous pipe's gap so it's fair. */
static void new_gap(pipe_t *p, int prev_gap_y)
{
    p->gap_h = gap_height();
    int lo = 50, hi = PLAY_H - 50 - p->gap_h;
    if (prev_gap_y >= 0) {
        if (lo < prev_gap_y - 140) lo = prev_gap_y - 140;
        if (hi > prev_gap_y + 140) hi = prev_gap_y + 140;
    }
    p->gap_y = lo + (int)(game_random() % (uint32_t)(hi - lo + 1));
    p->passed = false;

    /* Bodies start 3 px off screen / below the grass (hiding those borders)
     * and tuck 3 px under their caps. */
    int bot_y = p->gap_y + p->gap_h + CAP_H;
    lv_obj_set_height(p->top, p->gap_y - CAP_H + 6);
    lv_obj_set_height(p->bot, PLAY_H - bot_y + 6);
}

static void place_pipe(const pipe_t *p)
{
    int x = (int)p->x;
    int cx = x - (CAP_W - PIPE_W) / 2;
    lv_obj_set_pos(p->top, x, -3);
    lv_obj_set_pos(p->top_cap, cx, p->gap_y - CAP_H);
    lv_obj_set_pos(p->bot_cap, cx, p->gap_y + p->gap_h);
    lv_obj_set_pos(p->bot, x, p->gap_y + p->gap_h + CAP_H - 3);
}

static void reset_pipes(void)
{
    int prev = -1;
    for (int i = 0; i < N_PIPES; i++) {
        pipe_t *p = &F.pipes[i];
        p->x = W + 120 + i * PIPE_SPACING;
        new_gap(p, prev);
        prev = p->gap_y;
        place_pipe(p);
    }
}

/* ---------- drawing ---------- */

static void ground_draw(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(F.ground, &a);

    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;

    d.bg_color = lv_color_hex(C_GROUND);
    lv_draw_rect(ctx, &d, &a);

    lv_area_t grass = { a.x1, a.y1, a.x2, a.y1 + 13 };
    d.bg_color = lv_color_hex(C_GRASS);
    lv_draw_rect(ctx, &d, &grass);

    /* Scrolling stripes on the grass. */
    d.bg_color = lv_color_hex(C_GRASS_DK);
    int off = (int)F.ground_off % 28;
    for (int x = a.x1 - off; x < a.x2; x += 28) {
        lv_area_t s = { x, a.y1 + 3, x + 13, a.y1 + 13 };
        lv_draw_rect(ctx, &d, &s);
    }

    lv_area_t edge = { a.x1, a.y1, a.x2, a.y1 + 2 };
    d.bg_color = lv_color_hex(0x543847);
    lv_draw_rect(ctx, &d, &edge);
    lv_area_t under = { a.x1, a.y1 + 14, a.x2, a.y1 + 16 };
    d.bg_color = lv_color_hex(0xC8BE6E);
    lv_draw_rect(ctx, &d, &under);
}

static void blob(lv_obj_t *parent, int x, int y, int d, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* Static clouds and bushes along the horizon. */
static void make_scenery(lv_obj_t *parent)
{
    for (int x = -30; x < W; x += 70) {
        int d = 80 + (x * 37 % 41 + 41) % 41;
        blob(parent, x, PLAY_H - 70 - d / 3, d, 0xE4F6EE);
    }
    for (int x = -20; x < W; x += 46) {
        int d = 50 + (x * 53 % 23 + 23) % 23;
        blob(parent, x, PLAY_H - d / 2 - 4, d, 0x5DBE4A);
    }
}

static void update_score_label(void)
{
    lv_label_set_text_fmt(F.score_lbl, "%d", F.score);
    lv_label_set_text_fmt(F.score_shadow, "%d", F.score);
}

static void place_bird(float angle_deci_deg, int frame)
{
    lv_img_set_src(F.bird, frame ? &img_flappy_bird_1 : &img_flappy_bird_0);
    lv_img_set_angle(F.bird, (int16_t)angle_deci_deg);
    lv_obj_set_pos(F.bird, BIRD_X - 24, (int)F.y - 18);
}

/* ---------- states ---------- */

static void go_ready(void)
{
    F.state = ST_READY;
    F.t = 0;
    F.y = PLAY_H * 0.45f;
    F.vy = 0;
    F.score = 0;
    F.new_best = false;
    update_score_label();
    reset_pipes();
    lv_label_set_text_fmt(F.hint, "Tap to fly!\nBest: %d", F.best);
    lv_obj_clear_flag(F.hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(F.panel, LV_OBJ_FLAG_HIDDEN);
}

static void game_over(void)
{
    F.state = ST_OVER;
    F.t = 0;
    if (F.score > F.best) {
        F.best = F.score;
        F.new_best = true;
        dash_set_int("flappy_best", F.best);
    }
    lv_label_set_text_fmt(F.panel_text, "Score: %d\nBest: %d%s\n\nTap to play again",
                          F.score, F.best, F.new_best ? "  (new!)" : "");
    lv_obj_clear_flag(F.panel, LV_OBJ_FLAG_HIDDEN);
}

static bool hits_pipe(void)
{
    for (int i = 0; i < N_PIPES; i++) {
        const pipe_t *p = &F.pipes[i];
        /* Body width (the caps stick out a little; ignoring that is kinder). */
        float left = p->x, right = p->x + PIPE_W;
        if (BIRD_X + BIRD_R <= left || BIRD_X - BIRD_R >= right) continue;
        if (F.y - BIRD_R < p->gap_y || F.y + BIRD_R > p->gap_y + p->gap_h) return true;
    }
    return false;
}

static void tick(lv_timer_t *t)
{
    (void)t;
    int64_t now = game_time_us();
    float dt = (now - F.last_us) / 1e6f;
    F.last_us = now;
    if (dt > 0.05f) dt = 0.05f;          /* after a hiccup, don't jump ahead */
    F.t += dt;

    int frame = ((int)(F.t * 10)) & 1;   /* wing flaps 5 times a second */

    switch (F.state) {
    case ST_READY:
        F.y = PLAY_H * 0.45f + sinf(F.t * 5) * 8;
        F.ground_off += 120 * dt;
        place_bird(0, frame);
        break;

    case ST_PLAYING: {
        F.vy += GRAVITY * dt;
        if (F.vy > MAX_VY) F.vy = MAX_VY;
        F.y += F.vy * dt;
        if (F.y < BIRD_R) {              /* the sky is a ceiling */
            F.y = BIRD_R;
            F.vy = 0;
        }

        float dx = pipe_speed() * dt;
        F.ground_off += dx;
        float max_x = 0;
        for (int i = 0; i < N_PIPES; i++) {
            if (F.pipes[i].x > max_x) max_x = F.pipes[i].x;
        }
        for (int i = 0; i < N_PIPES; i++) {
            pipe_t *p = &F.pipes[i];
            p->x -= dx;
            if (!p->passed && p->x + PIPE_W < BIRD_X - BIRD_R) {
                p->passed = true;
                F.score++;
                update_score_label();
            }
            if (p->x + CAP_W < 0) {      /* off screen: reuse on the right */
                int prev = F.pipes[(i + N_PIPES - 1) % N_PIPES].gap_y;
                p->x = max_x + PIPE_SPACING;
                new_gap(p, prev);
            }
            place_pipe(p);
        }

        float angle = F.vy * 900 / MAX_VY;
        if (angle < -250) angle = -250;
        place_bird(angle, frame);

        if (F.y + BIRD_R >= PLAY_H || hits_pipe()) {
            F.state = ST_FALLING;
            F.t = 0;
            if (F.vy < 0) F.vy = 0;
        }
        break;
    }

    case ST_FALLING:
        F.vy += GRAVITY * dt;
        if (F.vy > MAX_VY) F.vy = MAX_VY;
        F.y += F.vy * dt;
        if (F.y + BIRD_R >= PLAY_H) {
            F.y = PLAY_H - BIRD_R;
            game_over();
        }
        place_bird(900, 0);
        break;

    case ST_OVER:
        return;                          /* nothing moves */
    }
    lv_obj_invalidate(F.ground);
}

static void tapped(lv_event_t *e)
{
    (void)e;
    switch (F.state) {
    case ST_READY:
        F.state = ST_PLAYING;
        F.t = 0;
        lv_obj_add_flag(F.hint, LV_OBJ_FLAG_HIDDEN);
        F.vy = FLAP_VY;
        break;
    case ST_PLAYING:
        F.vy = FLAP_VY;
        break;
    case ST_OVER:
        if (F.t > 0.7f) go_ready();      /* ignore taps right after crashing */
        break;
    default:
        break;
    }
}

/* ---------- app entry points ---------- */

static lv_obj_t *big_label(lv_obj_t *parent, uint32_t color, int dx, int dy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "0");
    lv_obj_align(l, LV_ALIGN_TOP_MID, dx, 14 + dy);
    return l;
}

static void flappy_create(lv_obj_t *parent)
{
    memset(&F, 0, sizeof(F));
    F.best = dash_get_int("flappy_best", 0);

    lv_obj_set_style_bg_color(parent, lv_color_hex(C_SKY), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    make_scenery(parent);

    for (int i = 0; i < N_PIPES; i++) {
        pipe_t *p = &F.pipes[i];
        p->top = pipe_part(parent, false);
        p->bot = pipe_part(parent, false);
        p->top_cap = pipe_part(parent, true);
        p->bot_cap = pipe_part(parent, true);
    }

    lv_obj_update_layout(parent);
    F.ground = lv_obj_create(parent);
    lv_obj_remove_style_all(F.ground);
    lv_obj_set_size(F.ground, W, lv_obj_get_height(parent) - PLAY_H);
    lv_obj_set_pos(F.ground, 0, PLAY_H);
    lv_obj_clear_flag(F.ground, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(F.ground, ground_draw, LV_EVENT_DRAW_MAIN, NULL);

    F.bird = lv_img_create(parent);
    lv_img_set_src(F.bird, &img_flappy_bird_0);
    lv_img_set_pivot(F.bird, 24, 18);

    F.score_shadow = big_label(parent, 0x2B2B2B, 3, 3);
    F.score_lbl = big_label(parent, 0xFFFFFF, 0, 0);

    F.hint = lv_label_create(parent);
    lv_obj_set_style_text_font(F.hint, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(F.hint, lv_color_white(), 0);
    lv_obj_set_style_text_align(F.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(F.hint, lv_color_hex(0x1E252C), 0);
    lv_obj_set_style_bg_opa(F.hint, LV_OPA_60, 0);
    lv_obj_set_style_radius(F.hint, 14, 0);
    lv_obj_set_style_pad_hor(F.hint, 24, 0);
    lv_obj_set_style_pad_ver(F.hint, 12, 0);
    lv_obj_align(F.hint, LV_ALIGN_CENTER, 120, 20);

    F.panel = lv_obj_create(parent);
    lv_obj_set_size(F.panel, 340, LV_SIZE_CONTENT);
    lv_obj_align(F.panel, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_radius(F.panel, 18, 0);
    lv_obj_set_style_bg_color(F.panel, lv_color_hex(0xFFF3D6), 0);
    lv_obj_set_style_border_color(F.panel, lv_color_hex(0x543847), 0);
    lv_obj_set_style_border_width(F.panel, 4, 0);
    lv_obj_set_style_pad_all(F.panel, 20, 0);
    lv_obj_set_flex_flow(F.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(F.panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(F.panel, 10, 0);
    lv_obj_clear_flag(F.panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(F.panel);
    lv_label_set_text(title, "Game over");
    lv_obj_set_style_text_font(title, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xD84315), 0);
    F.panel_text = lv_label_create(F.panel);
    lv_obj_set_style_text_font(F.panel_text, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(F.panel_text, lv_color_hex(0x543847), 0);
    lv_obj_set_style_text_align(F.panel_text, LV_TEXT_ALIGN_CENTER, 0);

    /* Invisible layer on top that turns any tap into a flap. */
    lv_obj_t *touch = lv_obj_create(parent);
    lv_obj_remove_style_all(touch);
    lv_obj_set_size(touch, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, tapped, LV_EVENT_PRESSED, NULL);

    go_ready();
    place_bird(0, 0);
    F.last_us = game_time_us();
    F.timer = lv_timer_create(tick, 16, NULL);
}

static void flappy_destroy(void)
{
    if (F.timer) {
        lv_timer_del(F.timer);
    }
    memset(&F, 0, sizeof(F));
}

int app_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    static const dash_app_def_t def = {
        .api_version = DASH_API_VERSION, .create = flappy_create, .destroy = flappy_destroy,
    };
    dash_app_register(&def);
    return 0;
}
