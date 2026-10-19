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
 * Apps with pin_last = true (Settings) are always drawn after all the
 * others, wherever they sit in s_apps[].
 *
 * Folders: an app_t with `folder` set (a NULL-terminated list of apps) is
 * a folder tile; tapping it opens a grid of those apps. Folders are defined
 * in launcher.c next to s_apps[] (e.g. "Games").
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
    /* true = always show this tile at the end of the home screen. */
    bool pin_last;
    /* Non-NULL = this tile is a folder of these apps (NULL-terminated);
     * create/destroy are then unused. */
    const struct app_s *const *folder;
} app_t;

/* ---- Registered apps ---- */
extern const app_t world_clock_app;
extern const app_t weather_app;
extern const app_t chess_app;
extern const app_t checkers_app;
extern const app_t flappy_app;
extern const app_t snake_app;
extern const app_t g2048_app;
extern const app_t settings_app;

/* Weather locations, so Settings can offer a default-city choice. */
int         weather_location_count(void);
const char *weather_location_name(int index);

/* ---- Shared fonts ----
 * Uses the biggest Montserrat sizes enabled in menuconfig, falling back
 * to smaller ones (or the default font) so the code always compiles.
 */
#if LV_FONT_MONTSERRAT_48
#define APP_FONT_HUGE (&lv_font_montserrat_48)
#elif LV_FONT_MONTSERRAT_36
#define APP_FONT_HUGE (&lv_font_montserrat_36)
#elif LV_FONT_MONTSERRAT_28
#define APP_FONT_HUGE (&lv_font_montserrat_28)
#else
#define APP_FONT_HUGE LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_28
#define APP_FONT_LARGE (&lv_font_montserrat_28)
#elif LV_FONT_MONTSERRAT_24
#define APP_FONT_LARGE (&lv_font_montserrat_24)
#elif LV_FONT_MONTSERRAT_20
#define APP_FONT_LARGE (&lv_font_montserrat_20)
#else
#define APP_FONT_LARGE LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_20
#define APP_FONT_MEDIUM (&lv_font_montserrat_20)
#elif LV_FONT_MONTSERRAT_18
#define APP_FONT_MEDIUM (&lv_font_montserrat_18)
#elif LV_FONT_MONTSERRAT_16
#define APP_FONT_MEDIUM (&lv_font_montserrat_16)
#else
#define APP_FONT_MEDIUM LV_FONT_DEFAULT
#endif

/* Shared colours */
#define APP_COLOR_BG       0x101418
#define APP_COLOR_PANEL    0x1E252C
#define APP_COLOR_TEXT     0xFFFFFF
#define APP_COLOR_SUBTEXT  0x9AA5B1
