#pragma once
/*
 * Tiny "app" framework for the launcher.
 *
 * Each app provides a name, an icon (an LV_SYMBOL_* string), a tile colour,
 * a create() function that builds its UI inside the given parent object,
 * and an optional destroy() function that cleans up timers etc.
 * The launcher deletes the app's LVGL objects for you.
 *
 * To add a new app:
 *   1. Write app_myapp.c defining `const app_t my_app = {...};`
 *   2. Add `extern const app_t my_app;` below
 *   3. Add `&my_app` to the s_apps[] list in launcher.c
 *   4. Add app_myapp.c to SRCS in main/CMakeLists.txt
 *
 * Apps with pin_last = true (Settings) start after all the others,
 * wherever they sit in s_apps[]; the user can still move them.
 *
 * Folders: an app_t with `folder` set (a NULL-terminated list of apps) is
 * a folder tile; tapping it opens a grid of those apps. Folders are defined
 * in launcher.c next to s_apps[]. The games are SD card apps (apps/ in the
 * project) that ask for the "Games" folder.
 */
#include "lvgl.h"

typedef struct app_s {
    const char *name;
    const char *symbol;      /* e.g. LV_SYMBOL_BELL; shown on the tile */
    uint32_t    color_hex;   /* tile colour, e.g. 0x2E7D32 */
    void (*create)(lv_obj_t *parent);
    void (*destroy)(void);
    /* Optional: draws a custom tile icon into a size x size box instead of
     * using `symbol`. */
    void (*draw_icon)(lv_obj_t *holder, int size);
    /* Optional: a picture for the tile instead of `symbol` (SD card apps). */
    const lv_img_dsc_t *icon_img;
    /* true = start this tile at the end (until the user moves it). */
    bool pin_last;
    /* Non-NULL = this tile is a folder of these apps (NULL-terminated);
     * create/destroy are then unused. */
    const struct app_s *const *folder;
} app_t;

/* ---- Registered apps ---- */
extern const app_t world_clock_app;
extern const app_t weather_app;
extern const app_t stonks_app;
extern const app_t photos_app;
extern const app_t files_app;
extern const app_t timers_app;
extern const app_t jellyfin_app;
extern const app_t dashpad_app;
extern const app_t monitor_app;
extern const app_t settings_app;

/* Weather locations, so Settings can offer a default-city choice. */
int         weather_location_count(void);
const char *weather_location_name(int index);

/* ---- Info ticker sources (see ticker.c) ----
 * *_ticker_items fill up to `max` items of text (LVGL recolor markup,
 * "#RRGGBB text#") and return how many. *_background_tick keep the data
 * fresh while the app is closed; the ticker calls them about once a second
 * (LVGL task). */
#define TICKER_ITEM_LEN 320
int  weather_ticker_items(char (*out)[TICKER_ITEM_LEN], int max);
void weather_background_tick(void);
int  stonks_ticker_items(char (*out)[TICKER_ITEM_LEN], int max);
void stonks_background_tick(void);
/* The server monitor's item (monitor.c; its own option, not in Settings). */
int  monitor_ticker_items(char (*out)[TICKER_ITEM_LEN], int max);

/* ---- Shared fonts and colours (also used by SD card apps) ---- */
#include "dash_style.h"
