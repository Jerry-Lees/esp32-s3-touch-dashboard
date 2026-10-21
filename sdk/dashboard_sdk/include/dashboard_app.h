#pragma once
/*
 * Dashboard app SDK: what an SD card app (.app file) can call besides LVGL.
 *
 * An app is built as an ELF file (see apps/README.md). Its entry point,
 * app_main(), runs once when the app is opened and must register the app:
 *
 *     static void create(lv_obj_t *parent) { ... build the UI in parent ... }
 *     static void destroy(void) { ... delete timers, free memory ... }
 *
 *     int app_main(int argc, char *argv[])
 *     {
 *         static const dash_app_def_t def = {
 *             .api_version = DASH_API_VERSION, .create = create, .destroy = destroy,
 *         };
 *         dash_app_register(&def);
 *         return 0;
 *     }
 *
 * create() gets the 800 x 416 area below the top bar; the dashboard deletes
 * the objects in it after destroy(). Everything runs in the LVGL task, so
 * LVGL calls are safe in create(), event callbacks and lv_timers. The app's
 * code is unloaded after it's closed: leave nothing running (timers, tasks)
 * that points into it.
 *
 * Available: the LVGL 8.4 API (including the Montserrat 14/20/28/48 fonts),
 * common C library functions (string, stdio formatting, malloc, time, math
 * such as sinf/cosf), the functions below, the dashboard's fonts and colours
 * (dash_style.h) and the game kit (game_common.h: board widget, background
 * worker, dialogs, swipe input).
 *
 * Versions: 1 = the first API; 2 adds dash_get_int/dash_set_int, dash_keep
 * and the game kit. The display runs apps made for its version or older.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_API_VERSION 2

typedef struct {
    uint32_t api_version;               /* DASH_API_VERSION */
    void (*create)(lv_obj_t *parent);
    void (*destroy)(void);              /* may be NULL */
} dash_app_def_t;

/* Call from app_main(). */
void dash_app_register(const dash_app_def_t *def);

/* A message banner at the top of the screen for ms milliseconds. */
void dash_toast(const char *text, uint32_t ms);

/* True once the clock is set (from the internet or the clock chip); until
 * then time() is wrong. localtime_r() gives the display's local time zone. */
bool dash_time_is_set(void);

/* The user's 12/24-hour choice (Settings > Time). */
bool dash_clock_24h(void);

/* Memory in PSRAM (8 MB; good for image buffers). Free with dash_free(). */
void *dash_alloc(size_t size);
void  dash_free(void *p);

/* Wall-clock time in milliseconds since 1970 (for smooth animations). */
int64_t dash_now_ms(void);

/* ---- API 2 ---- */

/* Saved numbers (kept across restarts and in the Settings backup). Keys are
 * 1-14 characters; start them with your app's name ("snake_best") so they
 * don't clash with other apps. dash_get_int returns def if nothing's saved. */
int32_t dash_get_int(const char *key, int32_t def);
void    dash_set_int(const char *key, int32_t value);

/* A block of PSRAM that stays until the display restarts, for state that
 * should survive closing the app (a game in progress). The first call gives
 * zeroed memory; later ones with the same name and size give the same block
 * back (a new size starts over). Each app has its own names. At most 8 per
 * app and 256 KB each; NULL if that's exceeded or memory is short.
 * Don't keep pointers to your app's code or static data in it: the app may
 * be loaded at another address next time. */
void *dash_keep(const char *name, size_t size);

#ifdef __cplusplus
}
#endif
