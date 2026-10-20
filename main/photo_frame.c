/*
 * Photo slideshow engine (see photo_frame.h).
 *
 * Worker task: scans dashboard/pictures, and decodes one picture at a time
 * into a w x h RGB565 frame (letterboxed or cropped, rotated per EXIF).
 * LVGL side: two lv_img objects; a new picture goes into the hidden one,
 * which fades in on top. Three frame buffers: the one on screen, the one
 * fading out, and the one being decoded. They're allocated once (for the
 * largest view) and kept, so closing never has to wait for a decode: the
 * worker runs jobs in order, so a leftover decode finishes before any new
 * one. A generation counter drops results from a closed view.
 */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "lvgl.h"
#include "src/extra/libs/sjpg/tjpgd.h"

#include "app_framework.h"
#include "launcher.h"
#include "photo_frame.h"
#include "pic_decode.h"
#include "sd_card.h"
#include "settings.h"

static const char *TAG = "photos";

#define PICS_DIR    SD_DIR "/pictures"
#define MAX_FILES   1000
#define PATH_LEN    160
#define MAX_SRC_PX  (1200 * 1000)     /* decoded (pre-resize) pixel budget */
#define FADE_MS     600

static const int INTERVALS[] = { 5, 10, 30, 60, 300 };
#define N_INTERVALS (int)(sizeof(INTERVALS) / sizeof(INTERVALS[0]))

/* ======================= worker task ======================= */

typedef enum { JOB_SCAN, JOB_DECODE, JOB_DELETE } job_kind_t;

typedef struct {
    job_kind_t kind;
    int        gen;
    int        file;               /* JOB_DECODE/DELETE: index into s_files */
    uint16_t  *buf;                /* JOB_DECODE: w * h RGB565 */
    int        w, h;
    bool       fill;
} job_t;

typedef struct {
    job_kind_t kind;
    int        gen;
    int        file;
    int        fb;                 /* which frame buffer (LVGL side bookkeeping) */
    bool       ok;
    int        count;              /* JOB_SCAN: files found */
    char       err[64];
} result_t;

static QueueHandle_t s_jobs, s_results;
static char (*s_files)[PATH_LEN];  /* PSRAM; written by the worker during a scan */

/* ---------- picture list ---------- */

bool photo_is_picture(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (name[0] == '.' || !dot) return false;          /* hidden / macOS "._" files */
    return !strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".bmp") ||
           !strcasecmp(dot, ".png") || !strcasecmp(dot, ".gif");
}

static void scan_dir(const char *dir, int depth, int *n)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && *n < MAX_FILES) {
        if (de->d_name[0] == '.') continue;
        char path[PATH_LEN];
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path)) continue;
        if (de->d_type == DT_DIR) {
            if (depth < 3) scan_dir(path, depth + 1, n);
        } else if (photo_is_picture(de->d_name)) {
            strlcpy(s_files[(*n)++], path, PATH_LEN);
        }
    }
    closedir(d);
}

static int cmp_path(const void *a, const void *b)
{
    return strcasecmp(a, b);
}

/* ---------- EXIF orientation (1..8, 1 = as stored) ---------- */

static int exif_orientation(FILE *f)
{
    uint8_t m[4];
    if (fread(m, 1, 2, f) != 2 || m[0] != 0xFF || m[1] != 0xD8) return 1;
    for (int seg = 0; seg < 32; seg++) {
        if (fread(m, 1, 4, f) != 4 || m[0] != 0xFF) return 1;
        int marker = m[1], len = (m[2] << 8) | m[3];
        if (marker == 0xDA || len < 2) return 1;          /* image data: no EXIF */
        if (marker != 0xE1) {
            fseek(f, len - 2, SEEK_CUR);
            continue;
        }
        int n = len - 2 > 65536 ? 65536 : len - 2;
        uint8_t *b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
        if (!b) return 1;
        int o = 1;
        if (fread(b, 1, n, f) == (size_t)n && n > 14 && memcmp(b, "Exif\0\0", 6) == 0) {
            const uint8_t *t = b + 6;
            int tn = n - 6;
            bool le = t[0] == 'I';
#define RD16(p) (le ? (p)[0] | (p)[1] << 8 : (p)[0] << 8 | (p)[1])
#define RD32(p) (le ? (uint32_t)RD16(p) | (uint32_t)RD16((p) + 2) << 16 \
                    : (uint32_t)RD16(p) << 16 | (uint32_t)RD16((p) + 2))
            uint32_t ifd = RD32(t + 4);
            if (ifd + 2 <= (uint32_t)tn) {
                int cnt = RD16(t + ifd);
                for (int i = 0; i < cnt; i++) {
                    uint32_t e = ifd + 2 + i * 12;
                    if (e + 12 > (uint32_t)tn) break;
                    if (RD16(t + e) == 0x0112) {
                        o = RD16(t + e + 8);
                        break;
                    }
                }
            }
#undef RD16
#undef RD32
        }
        free(b);
        return o >= 1 && o <= 8 ? o : 1;
    }
    return 1;
}

/* ---------- scaled source image ---------- */

typedef pic_img_t img_t;            /* RGB565, PSRAM */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
}

/* Picks the largest 1/2^s reduction that still has enough pixels for the
 * view (ow x oh = picture size after rotation). */
static int pick_scale(int ow, int oh, int w, int h, bool fill)
{
    float k = fill ? fmaxf((float)w / ow, (float)h / oh)
                   : fminf((float)w / ow, (float)h / oh);
    int s = 0;
    while (s < 3 && k * (1 << (s + 1)) <= 1.0f) s++;
    while (s < 3 && (long)(ow >> s) * (oh >> s) > MAX_SRC_PX) s++;
    return s;
}

/* JPEG via TJpgDec, reading straight from the file. */
typedef struct {
    FILE *f;
    img_t *img;
} jctx_t;

static size_t jd_in(JDEC *jd, uint8_t *buf, size_t n)
{
    jctx_t *c = jd->device;
    if (!buf) return fseek(c->f, n, SEEK_CUR) == 0 ? n : 0;
    return fread(buf, 1, n, c->f);
}

static int jd_out(JDEC *jd, void *data, JRECT *r)
{
    jctx_t *c = jd->device;
    const uint8_t *p = data;
    for (int y = r->top; y <= r->bottom; y++) {
        for (int x = r->left; x <= r->right; x++, p += 3) {
            if (x < c->img->w && y < c->img->h) {
                c->img->px[y * c->img->w + x] = rgb565(p[0], p[1], p[2]);
            }
        }
    }
    return 1;
}

static bool load_jpeg(FILE *f, img_t *img, int *orient, int w, int h, bool fill, char *err)
{
    *orient = exif_orientation(f);
    rewind(f);
    void *pool = heap_caps_malloc(8192, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pool) return false;
    JDEC jd;
    jctx_t c = { .f = f, .img = img };
    JRESULT r = jd_prepare(&jd, jd_in, pool, 8192, &c);
    bool ok = false;
    if (r == JDR_FMT3) {
        strcpy(err, "progressive JPEG (not supported)");
    } else if (r != JDR_OK) {
        snprintf(err, 64, "not a readable JPEG (%d)", r);
    } else {
        bool rot = *orient >= 5;
        int s = pick_scale(rot ? jd.height : jd.width, rot ? jd.width : jd.height, w, h, fill);
        img->w = (jd.width + (1 << s) - 1) >> s;
        img->h = (jd.height + (1 << s) - 1) >> s;
        img->px = heap_caps_calloc((size_t)img->w * img->h, 2, MALLOC_CAP_SPIRAM);
        if (!img->px) {
            strcpy(err, "too big for memory");
        } else if ((r = jd_decomp(&jd, jd_out, s)) != JDR_OK) {
            snprintf(err, 64, "damaged JPEG (%d)", r);
        } else {
            ok = true;
        }
    }
    free(pool);
    return ok;
}

/* 24- or 32-bit uncompressed BMP (e.g. our own screenshots). */
static bool load_bmp(FILE *f, img_t *img, int w, int h, bool fill, char *err)
{
    uint8_t hd[54];
    if (fread(hd, 1, 54, f) != 54 || hd[0] != 'B' || hd[1] != 'M') {
        strcpy(err, "not a BMP file");
        return false;
    }
#define U16(o) (hd[o] | hd[(o) + 1] << 8)
#define U32(o) ((uint32_t)U16(o) | (uint32_t)U16((o) + 2) << 16)
    uint32_t off = U32(10);
    int32_t bw = (int32_t)U32(18), bh = (int32_t)U32(22);
    int bpp = U16(28), comp = U32(30);
#undef U16
#undef U32
    bool top_down = bh < 0;
    if (bh < 0) bh = -bh;
    if ((bpp != 24 && bpp != 32) || comp != 0 || bw <= 0 || bh <= 0 || bw > 20000 || bh > 20000) {
        strcpy(err, "only 24/32-bit uncompressed BMP");
        return false;
    }
    int s = pick_scale(bw, bh, w, h, fill), step = 1 << s;
    img->w = (bw + step - 1) / step;
    img->h = (bh + step - 1) / step;
    img->px = heap_caps_calloc((size_t)img->w * img->h, 2, MALLOC_CAP_SPIRAM);
    size_t row = ((size_t)bw * (bpp / 8) + 3) & ~3u;
    uint8_t *line = heap_caps_malloc(row, MALLOC_CAP_SPIRAM);
    bool ok = img->px && line;
    for (int y = 0; ok && y < img->h; y++) {
        int sy = y * step;
        long file_row = top_down ? sy : bh - 1 - sy;
        ok = fseek(f, off + file_row * row, SEEK_SET) == 0 && fread(line, 1, row, f) == row;
        for (int x = 0; ok && x < img->w; x++) {
            const uint8_t *p = line + (size_t)x * step * (bpp / 8);
            img->px[y * img->w + x] = rgb565(p[2], p[1], p[0]);
        }
    }
    if (!ok && !err[0]) strcpy(err, "couldn't read the BMP");
    free(line);
    return ok;
}

/* ---------- resize + rotate into the w x h frame ---------- */

static inline uint16_t lerp565(uint16_t a, uint16_t b, uint16_t c, uint16_t d, int fx, int fy)
{
    /* fx, fy in 0..256 */
    int r = (((a >> 11) * (256 - fx) + (b >> 11) * fx) * (256 - fy) +
             ((c >> 11) * (256 - fx) + (d >> 11) * fx) * fy) >> 16;
    int g = ((((a >> 5) & 63) * (256 - fx) + ((b >> 5) & 63) * fx) * (256 - fy) +
             (((c >> 5) & 63) * (256 - fx) + ((d >> 5) & 63) * fx) * fy) >> 16;
    int bl = (((a & 31) * (256 - fx) + (b & 31) * fx) * (256 - fy) +
              ((c & 31) * (256 - fx) + (d & 31) * fx) * fy) >> 16;
    return r << 11 | g << 5 | bl;
}

static void render(const img_t *src, int o, uint16_t *dst, int w, int h, bool fill)
{
    memset(dst, 0, (size_t)w * h * 2);
    bool rot = o >= 5;
    int ow = rot ? src->h : src->w, oh = rot ? src->w : src->h;
    float k = fill ? fmaxf((float)w / ow, (float)h / oh) : fminf((float)w / ow, (float)h / oh);
    int dw = (int)lroundf(ow * k), dh = (int)lroundf(oh * k);
    int x0 = (w - dw) / 2, y0 = (h - dh) / 2;
    int ya = y0 < 0 ? 0 : y0, yb = y0 + dh > h ? h : y0 + dh;
    int xa = x0 < 0 ? 0 : x0, xb = x0 + dw > w ? w : x0 + dw;
    float inv = 1.0f / k;

    for (int y = ya; y < yb; y++) {
        float v = (y - y0 + 0.5f) * inv - 0.5f;
        for (int x = xa; x < xb; x++) {
            float u = (x - x0 + 0.5f) * inv - 0.5f;
            float sx, sy;                         /* (u, v) on screen -> source */
            switch (o) {
            case 2:  sx = ow - 1 - u; sy = v;          break;
            case 3:  sx = ow - 1 - u; sy = oh - 1 - v; break;
            case 4:  sx = u;          sy = oh - 1 - v; break;
            case 5:  sx = v;          sy = u;          break;
            case 6:  sx = v;          sy = src->h - 1 - u; break;
            case 7:  sx = src->w - 1 - v; sy = src->h - 1 - u; break;
            case 8:  sx = src->w - 1 - v; sy = u;      break;
            default: sx = u;          sy = v;          break;
            }
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            int ix = (int)sx, iy = (int)sy;
            if (ix >= src->w - 1) ix = src->w - 2 < 0 ? 0 : src->w - 2;
            if (iy >= src->h - 1) iy = src->h - 2 < 0 ? 0 : src->h - 2;
            int fx = (int)((sx - ix) * 256), fy = (int)((sy - iy) * 256);
            if (fx > 256) fx = 256;
            if (fy > 256) fy = 256;
            const uint16_t *p = src->px + iy * src->w + ix;
            int nx = src->w > 1 ? 1 : 0, ny = src->h > 1 ? src->w : 0;
            dst[y * w + x] = lerp565(p[0], p[nx], p[ny], p[ny + nx], fx, fy);
        }
    }
}

static int job_scale(int w, int h, const void *arg)
{
    const job_t *j = arg;
    return pick_scale(w, h, j->w, j->h, j->fill);
}

bool photo_decode_file(const char *path, uint16_t *buf, int w, int h, bool fill, char *err)
{
    err[0] = 0;
    const job_t job = { .buf = buf, .w = w, .h = h, .fill = fill }, *j = &job;
    FILE *f = fopen(path, "rb");
    if (!f) {
        strcpy(err, "couldn't open the file");
        return false;
    }
    setvbuf(f, NULL, _IOFBF, 16384);
    img_t img = {0};
    int orient = 1;
    const char *dot = strrchr(path, '.');
    bool ok;
    if (dot && !strcasecmp(dot, ".bmp"))      ok = load_bmp(f, &img, j->w, j->h, j->fill, err);
    else if (dot && !strcasecmp(dot, ".png")) ok = pic_load_png(f, &img, job_scale, j, err);
    else if (dot && !strcasecmp(dot, ".gif")) ok = pic_load_gif(f, &img, job_scale, j, err);
    else                                      ok = load_jpeg(f, &img, &orient, j->w, j->h, j->fill, err);
    fclose(f);
    if (ok) render(&img, orient, j->buf, j->w, j->h, j->fill);
    free(img.px);
    return ok;
}

static void worker(void *arg)
{
    (void)arg;
    job_t j;
    for (;;) {
        xQueueReceive(s_jobs, &j, portMAX_DELAY);
        result_t r = { .kind = j.kind, .gen = j.gen, .file = j.file };
        if (j.kind == JOB_SCAN) {
            int n = 0;
            if (sd_card_mounted()) scan_dir(PICS_DIR, 0, &n);
            qsort(s_files, n, PATH_LEN, cmp_path);
            r.count = n;
            r.ok = true;
        } else if (j.kind == JOB_DELETE) {
            /* Here, not in the LVGL task, so it never overlaps a decode. */
            r.ok = unlink(s_files[j.file]) == 0;
            if (!r.ok) ESP_LOGW(TAG, "couldn't delete %s", s_files[j.file]);
        } else {
            int64_t t0 = esp_log_timestamp();
            r.ok = photo_decode_file(s_files[j.file], j.buf, j.w, j.h, j.fill, r.err);
            if (r.ok) {
                ESP_LOGI(TAG, "%s in %d ms", s_files[j.file], (int)(esp_log_timestamp() - t0));
            } else {
                ESP_LOGW(TAG, "%s: %s", s_files[j.file], r.err);
            }
        }
        xQueueSend(s_results, &r, portMAX_DELAY);
    }
}

/* ======================= LVGL side ======================= */

static struct {
    lv_obj_t   *view, *img[2], *msg;
    lv_timer_t *timer;
    int         w, h;
    int         gen;
    pf_state_t  state;
    uint16_t   *fb[3];
    lv_img_dsc_t dsc[3];
    int         img_fb[2];         /* frame buffer shown by each lv_img, -1 = none */
    int         front;             /* lv_img on top */
    bool        fading;
    int         busy_fb;           /* frame buffer being decoded into, -1 = none */
    int         want;              /* order position being decoded, -1 = none */
    int         ready_pos, ready_fb; /* decoded and waiting to be shown (-1 = none) */
    int         dir;               /* direction to skip failed pictures */
    uint32_t    shown_ms;
    bool        paused;
    bool        deleted;           /* the picture on screen was deleted */
    int         fails;
} V;

static int *s_order;               /* PSRAM: display order -> file index */
static int  s_count;               /* pictures in the current scan */
static int  s_pos = -1;            /* position in s_order shown (kept between opens) */

static int wrap(int p) { return s_count ? ((p % s_count) + s_count) % s_count : 0; }

static void set_msg(const char *text)
{
    if (!V.msg) return;
    if (text) {
        lv_label_set_text(V.msg, text);
        lv_obj_clear_flag(V.msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(V.msg, LV_OBJ_FLAG_HIDDEN);
    }
}

static int free_fb(void)
{
    for (int i = 0; i < 3; i++) {
        if (i != V.img_fb[0] && i != V.img_fb[1] && i != V.busy_fb && i != V.ready_fb) return i;
    }
    return -1;
}

static void request(int pos)
{
    int fb = free_fb();
    if (fb < 0 || V.busy_fb >= 0 || s_count == 0) return;
    job_t j = { .kind = JOB_DECODE, .gen = V.gen, .file = s_order[wrap(pos)],
                .buf = V.fb[fb], .w = V.w, .h = V.h, .fill = photo_frame_fill() };
    if (xQueueSend(s_jobs, &j, 0) == pdTRUE) {
        V.busy_fb = fb;
        V.want = wrap(pos);
    }
}

static void fade_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void fade_done(lv_anim_t *a)
{
    (void)a;
    if (!V.view) return;
    int back = !V.front;
    lv_obj_add_flag(V.img[back], LV_OBJ_FLAG_HIDDEN);
    V.img_fb[back] = -1;                    /* its buffer is free again */
    V.fading = false;
}

static void show_ready(void)
{
    int fb = V.ready_fb, next = !V.front;
    V.ready_fb = -1;
    s_pos = V.ready_pos;
    V.ready_pos = -1;
    lv_img_cache_invalidate_src(&V.dsc[fb]);
    lv_img_set_src(V.img[next], &V.dsc[fb]);
    V.img_fb[next] = fb;
    lv_obj_clear_flag(V.img[next], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(V.img[next]);
    lv_obj_move_foreground(V.msg);
    set_msg(NULL);
    V.state = PF_SHOWING;
    V.shown_ms = lv_tick_get();
    V.dir = 1;                              /* after a "previous", carry on forwards */
    V.deleted = false;

    if (V.img_fb[V.front] < 0) {            /* first picture: no fade */
        lv_obj_set_style_opa(V.img[next], LV_OPA_COVER, 0);
        V.front = next;
        return;
    }
    V.front = next;
    V.fading = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, V.img[next]);
    lv_anim_set_exec_cb(&a, fade_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, FADE_MS);
    lv_anim_set_ready_cb(&a, fade_done);
    lv_anim_start(&a);
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* Reorders s_order (the file indices still in the slideshow): sorted by
 * name, or shuffled. Deleted pictures stay out. */
static void build_order(void)
{
    if (!photo_frame_shuffle()) {
        qsort(s_order, s_count, sizeof(int), cmp_int);
    } else {
        for (int i = s_count - 1; i > 0; i--) {
            int k = esp_random() % (i + 1), t = s_order[i];
            s_order[i] = s_order[k];
            s_order[k] = t;
        }
    }
}

static void handle(const result_t *r)
{
    if (r->kind == JOB_DELETE) {
        if (!r->ok) launcher_toast("Couldn't delete the picture from the SD card", 5000);
        return;
    }
    if (r->gen != V.gen) return;             /* from a view that was closed */
    if (r->kind == JOB_SCAN) {
        int old = s_count;
        s_count = r->count;
        if (s_count != old || s_pos >= s_count) s_pos = -1;
        for (int i = 0; i < s_count; i++) s_order[i] = i;
        build_order();
        if (s_count == 0) {
            V.state = sd_card_mounted() ? PF_EMPTY : PF_NO_CARD;
            set_msg(V.state == PF_EMPTY
                    ? "No pictures yet.\nCopy JPEG, PNG, GIF or BMP pictures into\n"
                      "dashboard/pictures on the SD card, "
                      "then restart the display."
                    : "No SD card.\nInsert one with photos in dashboard/pictures\n"
                      "and restart the display.");
            return;
        }
        V.dir = 1;
        request(s_pos < 0 ? 0 : s_pos);      /* resume where we left off */
        return;
    }
    V.busy_fb = -1;
    if (r->ok) {
        V.fails = 0;
        V.ready_fb = r->fb;
        V.ready_pos = V.want;
        if (V.img_fb[V.front] < 0) show_ready();   /* nothing on screen yet */
    } else {
        /* Skip pictures that can't be shown (but not forever). */
        if (++V.fails >= s_count) {
            V.state = PF_EMPTY;
            set_msg("None of the pictures in dashboard/pictures could be shown.\n"
                    "Use normal (baseline) JPEG, PNG, GIF or BMP files.");
            return;
        }
        request(V.want + V.dir);
    }
    V.want = -1;
}

static void tick(lv_timer_t *t)
{
    (void)t;
    result_t r;
    while (xQueueReceive(s_results, &r, 0) == pdTRUE) {
        /* r.fb isn't filled by the worker: it decoded into V.busy_fb. */
        r.fb = V.busy_fb;
        handle(&r);
    }
    if (V.state != PF_SHOWING || V.fading) return;

    /* Pre-decode the next picture while this one is on screen. */
    if (V.ready_fb < 0 && V.busy_fb < 0) request(s_pos + V.dir);

    /* A picture prepared for the other direction (the user skipped) is dropped. */
    if (V.ready_fb >= 0 && V.ready_pos != wrap(s_pos + V.dir)) {
        V.ready_fb = -1;
        V.ready_pos = -1;
        return;
    }
    bool due = !V.paused && lv_tick_elaps(V.shown_ms) >= (uint32_t)photo_frame_interval_s() * 1000;
    if (due && V.ready_fb >= 0) show_ready();
}

/* Frame buffers for the largest view (full screen), allocated on first use. */
static uint16_t *s_fb[3];

lv_obj_t *photo_frame_open(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    photo_frame_close();
    if (!s_jobs) {
        s_files = heap_caps_calloc(MAX_FILES, PATH_LEN, MALLOC_CAP_SPIRAM);
        s_order = heap_caps_calloc(MAX_FILES, sizeof(int), MALLOC_CAP_SPIRAM);
        s_jobs = xQueueCreate(4, sizeof(job_t));
        s_results = xQueueCreate(4, sizeof(result_t));
        xTaskCreate(worker, "photos", 8192, NULL, 2, NULL);
    }
    int gen = V.gen + 1;
    memset(&V, 0, sizeof(V));
    V.gen = gen;
    V.w = w;
    V.h = h;
    V.img_fb[0] = V.img_fb[1] = V.busy_fb = V.ready_fb = V.want = V.ready_pos = -1;
    V.dir = 1;
    V.state = PF_LOADING;

    V.view = lv_obj_create(parent);
    lv_obj_remove_style_all(V.view);
    lv_obj_set_size(V.view, w, h);
    lv_obj_set_style_bg_color(V.view, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(V.view, LV_OPA_COVER, 0);
    /* Taps go to the parent, which decides what they do. */
    lv_obj_clear_flag(V.view, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 3; i++) {
        if (!s_fb[i]) s_fb[i] = heap_caps_malloc(800 * 480 * 2, MALLOC_CAP_SPIRAM);
        V.fb[i] = s_fb[i];
        V.dsc[i] = (lv_img_dsc_t){
            .header.cf = LV_IMG_CF_TRUE_COLOR, .header.w = w, .header.h = h,
            .data_size = (uint32_t)w * h * 2, .data = (const uint8_t *)V.fb[i] };
    }
    for (int i = 0; i < 2; i++) {
        V.img[i] = lv_img_create(V.view);
        lv_obj_add_flag(V.img[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(V.img[i], LV_OBJ_FLAG_CLICKABLE);
    }
    V.msg = lv_label_create(V.view);
    lv_obj_set_style_text_font(V.msg, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(V.msg, lv_color_hex(APP_COLOR_SUBTEXT), 0);
    lv_obj_set_style_text_align(V.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(V.msg);
    set_msg(LV_SYMBOL_IMAGE "  Looking for pictures...");

    if (!V.fb[0] || !V.fb[1] || !V.fb[2] || w * h > 800 * 480) {
        V.state = PF_EMPTY;
        set_msg("Not enough memory for the slideshow.");
        return V.view;
    }
    job_t j = { .kind = JOB_SCAN, .gen = V.gen };
    xQueueSend(s_jobs, &j, 0);
    V.timer = lv_timer_create(tick, 100, NULL);
    return V.view;
}

void photo_frame_close(void)
{
    if (!V.view && !V.timer) return;
    if (V.timer) lv_timer_del(V.timer);
    for (int i = 0; i < 2; i++) {
        if (V.img[i]) lv_anim_del(V.img[i], fade_cb);
    }
    /* A decode still running finishes into a buffer nobody shows; its
     * result is dropped by the generation check. */
    int gen = V.gen + 1;
    memset(&V, 0, sizeof(V));               /* the caller deletes the objects */
    V.gen = gen;
    V.busy_fb = -1;
}

void photo_frame_step(int dir)
{
    if (!V.view || V.state != PF_SHOWING || s_count < 2) return;
    V.dir = dir < 0 ? -1 : 1;
    if (V.ready_fb >= 0 && V.ready_pos == wrap(s_pos + V.dir) && !V.fading) {
        show_ready();
        return;
    }
    /* Drop the prepared picture (wrong direction) and decode the right one. */
    V.ready_fb = -1;
    V.ready_pos = -1;
    if (V.busy_fb < 0 && !V.fading) request(s_pos + V.dir);
    V.shown_ms = lv_tick_get() - (uint32_t)photo_frame_interval_s() * 1000;  /* show when ready */
}

bool photo_frame_delete_current(void)
{
    if (!V.view || V.state != PF_SHOWING || s_pos < 0 || V.deleted) return false;
    job_t j = { .kind = JOB_DELETE, .file = s_order[s_pos] };
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) return false;

    /* Take it out of the order list. */
    int p = s_pos;
    memmove(&s_order[p], &s_order[p + 1], (size_t)(s_count - p - 1) * sizeof(int));
    s_count--;

    /* Drop anything decoded or decoding (positions have moved): bumping the
     * generation ignores the result. The worker runs jobs in order, so a
     * buffer it's still writing is done before it's used again. */
    V.gen++;
    V.busy_fb = V.ready_fb = V.ready_pos = V.want = -1;
    V.fails = 0;
    V.deleted = true;

    if (s_count == 0) {
        s_pos = -1;
        for (int i = 0; i < 2; i++) {
            lv_anim_del(V.img[i], fade_cb);
            lv_obj_add_flag(V.img[i], LV_OBJ_FLAG_HIDDEN);
            V.img_fb[i] = -1;
        }
        V.fading = false;
        V.state = PF_EMPTY;
        set_msg("No pictures left.\nCopy JPEG, PNG, GIF or BMP pictures into\n"
                "dashboard/pictures on the SD card, then restart the display.");
        return true;
    }
    /* Show the picture that moved into its place as soon as it's ready. */
    s_pos = p - 1;
    V.dir = 1;
    if (!V.fading) request(p);
    V.shown_ms = lv_tick_get() - (uint32_t)photo_frame_interval_s() * 1000;
    return true;
}

void photo_frame_set_paused(bool p)  { V.paused = p; V.shown_ms = lv_tick_get(); }
bool photo_frame_paused(void)        { return V.paused; }
pf_state_t photo_frame_state(void)   { return V.state; }

const char *photo_frame_name(void)
{
    if (V.state != PF_SHOWING || s_pos < 0 || V.deleted) return "";
    const char *p = s_files[s_order[s_pos]];
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

void photo_frame_position(int *n, int *total)
{
    *n = s_pos + 1;
    *total = s_count;
}

int photo_frame_interval_s(void)
{
    int i = settings_get_u8("ph_int", 1);
    return INTERVALS[i < N_INTERVALS ? i : 1];
}

void photo_frame_cycle_interval(void)
{
    int i = settings_get_u8("ph_int", 1);
    settings_set_u8("ph_int", (i + 1) % N_INTERVALS);
}

bool photo_frame_shuffle(void) { return settings_get_u8("ph_shuf", 0) != 0; }

void photo_frame_set_shuffle(bool on)
{
    settings_set_u8("ph_shuf", on);
    build_order();
}

bool photo_frame_fill(void)    { return settings_get_u8("ph_fill", 0) != 0; }
void photo_frame_set_fill(bool on) { settings_set_u8("ph_fill", on); }
