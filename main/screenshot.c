/*
 * Screenshots. With an SD card in, the long-press saves a 24-bit BMP to
 * dashboard/screenshots/. Without one, the screen goes over the serial
 * console instead. That format (read by
 * tools/screenshot.py) is plain text lines, so log messages from other
 * tasks can appear in between without breaking it:
 *
 *   SHOT-BEGIN <width> <height> <chunks>
 *   SHOT-<n>:<base64 of 768 bytes of RGB565 little-endian pixels>
 *   ...
 *   SHOT-END
 *
 * An 800x480 screen is 750 KB and takes a few seconds; the screen doesn't
 * update meanwhile. With no PC reading the port, the console drops the
 * text, so a stray long-press is harmless.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "launcher.h"
#include "net_time.h"
#include "screenshot.h"
#include "sd_card.h"

#define CHUNK 768   /* bytes per line; a multiple of 3 keeps base64 unpadded */

static const char *TAG = "screenshot";

/* Picks a new file name in dashboard/screenshots: the date and time when
 * the clock is set, otherwise the next free number. */
static void next_path(char *out, size_t len)
{
    if (net_time_is_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char stamp[24];
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        snprintf(out, len, SD_DIR "/screenshots/shot_%s.bmp", stamp);
        return;
    }
    struct stat st;
    for (int i = 1; i < 10000; i++) {
        snprintf(out, len, SD_DIR "/screenshots/shot_%04d.bmp", i);
        if (stat(out, &st) != 0) return;
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* Writes RGB565 pixels as a 24-bit BMP (rows bottom-up, padded to 4 bytes). */
static bool save_bmp(const char *path, const uint16_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int row = (w * 3 + 3) & ~3;
    uint8_t hdr[54] = { 'B', 'M' };
    put32(hdr + 2, 54 + row * h);                     /* file size */
    put32(hdr + 10, 54);                              /* pixel data offset */
    put32(hdr + 14, 40);                              /* BITMAPINFOHEADER */
    put32(hdr + 18, w);
    put32(hdr + 22, h);
    put16(hdr + 26, 1);                               /* planes */
    put16(hdr + 28, 24);                              /* bits per pixel */
    put32(hdr + 34, row * h);
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    uint8_t *line = heap_caps_calloc(1, row, MALLOC_CAP_SPIRAM);
    for (int y = h - 1; ok && line && y >= 0; y--) {
        const uint16_t *src = px + y * w;
        for (int x = 0; x < w; x++) {
            uint16_t v = src[x];
            uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            line[x * 3 + 0] = (b << 3) | (b >> 2);    /* BMP stores B, G, R */
            line[x * 3 + 1] = (g << 2) | (g >> 4);
            line[x * 3 + 2] = (r << 3) | (r >> 2);
        }
        ok = fwrite(line, 1, row, f) == (size_t)row;
    }
    ok = ok && line;
    free(line);
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(path);
    return ok;
}

/* Sends the pixels over the serial console for tools/screenshot.py. */
static void send_usb(const uint8_t *buf, const lv_img_dsc_t *dsc);

/* Blends the top layer (alarm alerts, the new-key warning, ...) over the
 * screen's picture, so full-screen alerts show in screenshots too. */
static void add_top_layer(uint16_t *px, int w, int h)
{
    lv_obj_t *top = lv_layer_top();
    if (lv_obj_get_child_cnt(top) == 0) return;

    /* A full-screen, opaque alert hides everything underneath: take the top
     * layer as it is (blending can let the screen show through faintly). */
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(top); i++) {
        lv_obj_t *c = lv_obj_get_child(top, i);
        if (!lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) && lv_obj_get_width(c) >= w &&
            lv_obj_get_height(c) >= h && lv_obj_get_style_bg_opa(c, 0) == LV_OPA_COVER) {
            lv_img_dsc_t dsc;
            uint32_t size = lv_snapshot_buf_size_needed(top, LV_IMG_CF_TRUE_COLOR);
            if (size == (uint32_t)w * h * 2) {
                lv_snapshot_take_to_buf(top, LV_IMG_CF_TRUE_COLOR, &dsc, px, size);
            }
            return;
        }
    }

    uint32_t size = lv_snapshot_buf_size_needed(top, LV_IMG_CF_TRUE_COLOR_ALPHA);
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    lv_img_dsc_t dsc;
    if (!buf || lv_snapshot_take_to_buf(top, LV_IMG_CF_TRUE_COLOR_ALPHA, &dsc, buf, size) != LV_RES_OK ||
        dsc.header.w != w || dsc.header.h != h) {
        free(buf);
        return;
    }
    for (int i = 0; i < w * h; i++) {
        const uint8_t *s = buf + i * 3;            /* RGB565 low, high, alpha */
        uint32_t a = s[2];
        if (a == 0) continue;
        uint16_t f = s[0] | s[1] << 8, b = px[i];
        uint32_t r = (((f >> 11) & 31) * a + ((b >> 11) & 31) * (255 - a)) / 255;
        uint32_t g = (((f >> 5) & 63) * a + ((b >> 5) & 63) * (255 - a)) / 255;
        uint32_t bl = ((f & 31) * a + (b & 31) * (255 - a)) / 255;
        px[i] = r << 11 | g << 5 | bl;
    }
    free(buf);
}

static void take(lv_timer_t *t)
{
    (void)t;
    lv_obj_t *scr = lv_scr_act();
    uint32_t size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory");
        return;
    }
    lv_img_dsc_t dsc;
    if (lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, size) != LV_RES_OK) {
        ESP_LOGE(TAG, "Snapshot failed");
        free(buf);
        return;
    }
    add_top_layer((uint16_t *)buf, dsc.header.w, dsc.header.h);

    if (sd_card_mounted()) {
        char path[96], msg[128];
        next_path(path, sizeof(path));
        if (save_bmp(path, (const uint16_t *)buf, dsc.header.w, dsc.header.h)) {
            snprintf(msg, sizeof(msg), "Screenshot saved: %s",
                     path + strlen(SD_MOUNT_POINT "/"));
        } else {
            snprintf(msg, sizeof(msg), "Couldn't save the screenshot to the SD card");
        }
        ESP_LOGI(TAG, "%s", msg);
        launcher_toast(msg, 3000);
    } else {
        send_usb(buf, &dsc);
    }
    free(buf);
}

static void send_usb(const uint8_t *buf, const lv_img_dsc_t *dsc_p)
{
    const lv_img_dsc_t dsc = *dsc_p;
    uint32_t chunks = (dsc.data_size + CHUNK - 1) / CHUNK;
    printf("\nSHOT-BEGIN %d %d %" PRIu32 "\n", (int)dsc.header.w, (int)dsc.header.h, chunks);
    char line[CHUNK / 3 * 4 + 1];
    for (uint32_t i = 0; i < chunks; i++) {
        uint32_t off = i * CHUNK;
        uint32_t n = dsc.data_size - off < CHUNK ? dsc.data_size - off : CHUNK;
        size_t olen = 0;
        mbedtls_base64_encode((unsigned char *)line, sizeof(line), &olen, buf + off, n);
        line[olen] = '\0';
        printf("SHOT-%" PRIu32 ":%s\n", i, line);
    }
    printf("SHOT-END\n");
    fflush(stdout);
}

static void long_pressed(lv_event_t *e)
{
    (void)e;
    /* Outside the input event, so the press is over before the snapshot. */
    lv_timer_t *t = lv_timer_create(take, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static bool s_requested;

static void request_poll(lv_timer_t *t)
{
    (void)t;
    if (__atomic_exchange_n(&s_requested, false, __ATOMIC_ACQ_REL)) take(NULL);
}

void screenshot_request(void)
{
    __atomic_store_n(&s_requested, true, __ATOMIC_RELEASE);
}

void screenshot_attach(lv_obj_t *obj)
{
    static lv_timer_t *poll;
    if (!poll) poll = lv_timer_create(request_poll, 200, NULL);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, long_pressed, LV_EVENT_LONG_PRESSED, NULL);
}
