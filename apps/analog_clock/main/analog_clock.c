/*
 * Analog Clock: an SD card app for the dashboard (and the example app).
 *
 * A 1970s-style clock: a brass bezel around a cream sunburst dial over
 * orange and mustard stripes, with the day and date on the left and the
 * digital time on the right. The red second hand sweeps smoothly.
 *
 * Drawing: everything that never moves (background, stripes, dial,
 * numerals) is drawn once into a full-size canvas. The hands are drawn on
 * top by a transparent object's draw event, and only the areas the hands
 * cover (old and new position) are redrawn each frame, so the sweep stays
 * smooth.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dashboard_app.h"

#define W        800
#define H        416
#define CX       400
#define CY       208
#define R_BEZEL  202                 /* outer edge of the brass ring */
#define R_DIAL   182                 /* cream face */
#define FPS_MS   33

/* 70s palette */
#define C_BG       0x2B1B12          /* dark chocolate */
#define C_ORANGE   0xD9622B
#define C_MUSTARD  0xE9A23B
#define C_CREAM    0xF4E6C4
#define C_CREAM2   0xEAD6A8
#define C_BROWN    0x3B2416
#define C_BRASS    0xC9993A
#define C_BRASS_HI 0xF0CF7A
#define C_BRASS_LO 0x6B4A1E
#define C_RED      0xC8102E

static struct {
    lv_obj_t   *canvas, *hands;
    lv_obj_t   *day, *date, *year, *time, *secs, *ampm;
    lv_timer_t *timer;
    lv_color_t *buf;
    float       a_hour, a_min, a_sec;      /* angles in degrees, 0 = 12 o'clock */
    lv_area_t   dirty_sec;                 /* where the second hand was drawn */
    lv_coord_t  ox, oy;                    /* app area's top-left on screen (drawing
                                              uses screen coordinates) */
    int         last_sec;
} C;

/* ---------- geometry ---------- */

static lv_point_t polar(float deg, float r)
{
    float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    lv_point_t p = { (lv_coord_t)lroundf(CX + r * cosf(a)), (lv_coord_t)lroundf(CY + r * sinf(a)) };
    return p;
}

/* Screen area covering a hand from radius r0 (negative = tail) to r1. */
static lv_area_t hand_area(float deg, float r0, float r1, int pad)
{
    lv_point_t a = polar(deg, r0), b = polar(deg, r1);
    lv_area_t ar = {
        .x1 = LV_MIN(a.x, b.x) - pad, .y1 = LV_MIN(a.y, b.y) - pad,
        .x2 = LV_MAX(a.x, b.x) + pad, .y2 = LV_MAX(a.y, b.y) + pad,
    };
    return ar;
}

/* ---------- the static picture (drawn once into the canvas) ---------- */

static void disc(int r, uint32_t color)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(color);
    lv_canvas_draw_rect(C.canvas, CX - r, CY - r, 2 * r + 1, 2 * r + 1, &d);
}

static void ring(int r, int width, uint32_t color)
{
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    lv_canvas_draw_arc(C.canvas, CX, CY, r, 0, 360, &d);
}

static void text_at(const char *txt, int x, int y, const lv_font_t *font, uint32_t color,
                    int spacing)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.color = lv_color_hex(color);
    d.letter_space = spacing;
    lv_point_t size;
    lv_txt_get_size(&size, txt, font, spacing, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_canvas_draw_text(C.canvas, x - size.x / 2, y - size.y / 2, size.x + 2, &d, txt);
}

static void draw_static(void)
{
    lv_canvas_fill_bg(C.canvas, lv_color_hex(C_BG), LV_OPA_COVER);

    /* Three stripes across the whole width, behind the dial. */
    static const uint32_t stripe[] = { C_ORANGE, C_MUSTARD, C_CREAM2 };
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    for (int i = 0; i < 3; i++) {
        r.bg_color = lv_color_hex(stripe[i]);
        lv_canvas_draw_rect(C.canvas, 0, CY - 52 + i * 36, W, 28, &r);
    }

    /* Bezel: dark edge, brass, a bright highlight, brass again, inner shadow. */
    disc(R_BEZEL, C_BRASS_LO);
    disc(R_BEZEL - 4, C_BRASS);
    ring(R_BEZEL - 9, 3, C_BRASS_HI);
    ring(R_BEZEL - 15, 2, C_BRASS_LO);
    disc(R_DIAL + 2, C_BROWN);

    /* Sunburst face: 48 wedges in two creams. */
    disc(R_DIAL, C_CREAM);
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_hex(C_CREAM2);
    for (int i = 0; i < 48; i += 2) {
        lv_point_t p[3] = { { CX, CY }, polar(i * 7.5f, R_DIAL - 1), polar((i + 1) * 7.5f, R_DIAL - 1) };
        lv_canvas_draw_polygon(C.canvas, p, 3, &r);
    }

    /* Chapter ring and minute track. */
    ring(R_DIAL - 30, 2, C_MUSTARD);
    ring(R_DIAL - 6, 1, C_BROWN);
    lv_draw_line_dsc_t l;
    lv_draw_line_dsc_init(&l);
    l.round_start = l.round_end = 1;
    for (int m = 0; m < 60; m++) {
        bool hour = m % 5 == 0;
        l.color = lv_color_hex(hour ? C_ORANGE : C_BROWN);
        l.width = hour ? 7 : 2;
        lv_point_t p[2] = { polar(m * 6.0f, hour ? R_DIAL - 28 : R_DIAL - 16),
                            polar(m * 6.0f, R_DIAL - 8) };
        lv_canvas_draw_line(C.canvas, p, 2, &l);
    }

    /* Numerals: 12, 3, 6, 9 big; the rest smaller. */
    for (int h = 1; h <= 12; h++) {
        char n[4];
        snprintf(n, sizeof(n), "%d", h);
        bool big = h % 3 == 0;
        lv_point_t p = polar(h * 30.0f, big ? R_DIAL - 62 : R_DIAL - 54);
        text_at(n, p.x, p.y, big ? &lv_font_montserrat_48 : &lv_font_montserrat_28,
                big ? C_BROWN : 0x7A4A22, 0);
    }

    /* Maker's marks. */
    text_at("DASHBOARD", CX, CY - 62, &lv_font_montserrat_14, C_BROWN, 4);
    text_at("QUARTZ  SWEEP", CX, CY + 58, &lv_font_montserrat_14, C_ORANGE, 3);
}

/* ---------- hands (drawn every frame) ---------- */

static void line(lv_draw_ctx_t *ctx, float deg, float r0, float r1, int width, uint32_t color,
                 lv_opa_t opa, int dx, int dy)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(color);
    d.width = width;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_point_t a = polar(deg, r0), b = polar(deg, r1);
    a.x += dx + C.ox; a.y += dy + C.oy;
    b.x += dx + C.ox; b.y += dy + C.oy;
    lv_draw_line(ctx, &d, &a, &b);
}

static void dot(lv_draw_ctx_t *ctx, lv_point_t c, int r, uint32_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    c.x += C.ox;
    c.y += C.oy;
    lv_area_t a = { c.x - r, c.y - r, c.x + r, c.y + r };
    lv_draw_rect(ctx, &d, &a);
}

static void hands_draw(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    const int s = 4;                                  /* shadow offset */
    lv_area_t me;
    lv_obj_get_coords(C.hands, &me);
    C.ox = me.x1;
    C.oy = me.y1;

    /* Shadows first. */
    line(ctx, C.a_hour, -16, 100, 13, 0x000000, LV_OPA_30, s, s);
    line(ctx, C.a_min, -20, 152, 9, 0x000000, LV_OPA_30, s, s);
    line(ctx, C.a_sec, -40, 166, 3, 0x000000, LV_OPA_30, s + 2, s + 2);

    /* Hour and minute hands: brown with a mustard inlay. */
    line(ctx, C.a_hour, -16, 100, 13, C_BROWN, LV_OPA_COVER, 0, 0);
    line(ctx, C.a_hour, 22, 88, 5, C_MUSTARD, LV_OPA_COVER, 0, 0);
    line(ctx, C.a_min, -20, 152, 9, C_BROWN, LV_OPA_COVER, 0, 0);
    line(ctx, C.a_min, 26, 140, 3, C_MUSTARD, LV_OPA_COVER, 0, 0);

    /* Second hand: red, with a round counterweight and a lollipop near the tip. */
    line(ctx, C.a_sec, -40, 166, 3, C_RED, LV_OPA_COVER, 0, 0);
    dot(ctx, polar(C.a_sec, -30), 9, C_RED, LV_OPA_COVER);
    dot(ctx, polar(C.a_sec, 128), 7, C_RED, LV_OPA_COVER);
    dot(ctx, polar(C.a_sec, 128), 3, C_CREAM, LV_OPA_COVER);

    /* Centre cap. */
    lv_point_t c = { CX, CY };
    dot(ctx, c, 13, C_BRASS, LV_OPA_COVER);
    dot(ctx, c, 9, C_BRASS_HI, LV_OPA_COVER);
    dot(ctx, c, 4, C_RED, LV_OPA_COVER);
}

/* ---------- time ---------- */

static lv_area_t sec_area(float deg)
{
    lv_area_t a = hand_area(deg, -40, 166, 16);
    return a;
}

static void invalidate(lv_area_t a)
{
    /* hands object sits at (0,0) of the app area; areas are relative to it */
    lv_area_t abs;
    lv_obj_get_coords(C.hands, &abs);
    a.x1 += abs.x1; a.x2 += abs.x1;
    a.y1 += abs.y1; a.y2 += abs.y1;
    lv_obj_invalidate_area(C.hands, &a);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    int64_t ms = dash_now_ms();
    time_t now = (time_t)(ms / 1000);
    struct tm tm;
    localtime_r(&now, &tm);
    bool set = dash_time_is_set();
    float sec = set ? tm.tm_sec + (ms % 1000) / 1000.0f : 0.0f;
    float min = set ? tm.tm_min + sec / 60.0f : 10.0f;
    float hour = set ? (tm.tm_hour % 12) + min / 60.0f : 10.0f;

    C.a_sec = sec * 6.0f;
    lv_area_t s = sec_area(C.a_sec);
    invalidate(C.dirty_sec);
    invalidate(s);
    C.dirty_sec = s;

    if (tm.tm_sec != C.last_sec || !set) {
        /* Once a second: hour and minute hands, and the side texts. */
        invalidate(hand_area(C.a_min, -20, 152, 14));
        invalidate(hand_area(C.a_hour, -16, 100, 16));
        C.a_min = min * 6.0f;
        C.a_hour = hour * 30.0f;
        invalidate(hand_area(C.a_min, -20, 152, 14));
        invalidate(hand_area(C.a_hour, -16, 100, 16));
        C.last_sec = tm.tm_sec;

        char b[32];
        if (set) {
            strftime(b, sizeof(b), "%A", &tm);
            for (char *p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
            b[3] = '\0';                                  /* MON, TUE ... */
            lv_label_set_text(C.day, b);
            strftime(b, sizeof(b), "%b %d", &tm);
            for (char *p = b; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
            lv_label_set_text(C.date, b);
            strftime(b, sizeof(b), "%Y", &tm);
            lv_label_set_text(C.year, b);
            if (dash_clock_24h()) {
                strftime(b, sizeof(b), "%H:%M", &tm);
                lv_label_set_text(C.ampm, "");
            } else {
                strftime(b, sizeof(b), "%I:%M", &tm);
                if (b[0] == '0') memmove(b, b + 1, strlen(b));
                lv_label_set_text(C.ampm, tm.tm_hour < 12 ? "AM" : "PM");
            }
            lv_label_set_text(C.time, b);
            snprintf(b, sizeof(b), ":%02d", tm.tm_sec);
            lv_label_set_text(C.secs, b);
        } else {
            lv_label_set_text(C.day, "---");
            lv_label_set_text(C.date, "NO TIME");
            lv_label_set_text(C.year, "YET");
            lv_label_set_text(C.time, "--:--");
            lv_label_set_text(C.secs, "");
            lv_label_set_text(C.ampm, "");
        }
    }
}

/* ---------- app ---------- */

static lv_obj_t *side_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int x,
                            int y, int spacing)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(l, spacing, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 200);
    lv_obj_set_pos(l, x - 100, y);
    lv_label_set_text(l, "");
    return l;
}

static void canvas_deleted(lv_event_t *e)
{
    (void)e;
    dash_free(C.buf);
    C.buf = NULL;
}

static void create(lv_obj_t *parent)
{
    memset(&C, 0, sizeof(C));
    C.last_sec = -1;
    C.buf = dash_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(W, H));
    if (!C.buf) {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, "Not enough memory for the clock face.");
        lv_obj_center(l);
        return;
    }
    C.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(C.canvas, C.buf, W, H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(C.canvas, 0, 0);
    lv_obj_add_event_cb(C.canvas, canvas_deleted, LV_EVENT_DELETE, NULL);
    draw_static();

    /* Left: day and date. Right: digital time. Above and below the stripes. */
    C.day  = side_label(parent, &lv_font_montserrat_48, C_MUSTARD, 100, 70, 6);
    C.date = side_label(parent, &lv_font_montserrat_28, C_CREAM, 100, 280, 3);
    C.year = side_label(parent, &lv_font_montserrat_20, C_ORANGE, 100, 318, 6);
    C.time = side_label(parent, &lv_font_montserrat_48, C_CREAM, 700, 70, 2);
    C.secs = side_label(parent, &lv_font_montserrat_28, C_ORANGE, 700, 280, 3);
    C.ampm = side_label(parent, &lv_font_montserrat_20, C_MUSTARD, 700, 318, 6);

    /* Transparent layer for the hands. */
    C.hands = lv_obj_create(parent);
    lv_obj_remove_style_all(C.hands);
    lv_obj_set_size(C.hands, W, H);
    lv_obj_set_pos(C.hands, 0, 0);
    lv_obj_clear_flag(C.hands, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(C.hands, hands_draw, LV_EVENT_DRAW_MAIN, NULL);

    C.dirty_sec = sec_area(0);
    tick(NULL);
    C.timer = lv_timer_create(tick, FPS_MS, NULL);
}

static void destroy(void)
{
    if (C.timer) lv_timer_del(C.timer);
    C.timer = NULL;
}

int app_main(int argc, char *argv[])
{
    static const dash_app_def_t def = {
        .api_version = DASH_API_VERSION,
        .create      = create,
        .destroy     = destroy,
    };
    dash_app_register(&def);
    return 0;
}
