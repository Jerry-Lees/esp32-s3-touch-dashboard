/*
 * Screenshot over the serial console. The format (read by
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

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/base64.h"

#include "screenshot.h"

#define CHUNK 768   /* bytes per line; a multiple of 3 keeps base64 unpadded */

static const char *TAG = "screenshot";

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
    free(buf);
}

static void long_pressed(lv_event_t *e)
{
    (void)e;
    /* Outside the input event, so the press is over before the snapshot. */
    lv_timer_t *t = lv_timer_create(take, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void screenshot_attach(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, long_pressed, LV_EVENT_LONG_PRESSED, NULL);
}
