#pragma once
#include <stdint.h>
/*
 * Starts Wi-Fi/time sync and shows the app launcher (home screen).
 * Call it once, while holding the LVGL lock, in place of lv_demo_music().
 */
void launcher_start(void);

/* Shows or hides LVGL's FPS / CPU meter to match settings_get_show_fps(). */
void launcher_apply_fps(void);

/* Shows or hides the home screen's info ticker to match settings_get_ticker(). */
void launcher_apply_ticker(void);

/* Shows a short message banner over whatever is on screen for ms
 * milliseconds (tap to dismiss). LVGL task only. */
void launcher_toast(const char *text, uint32_t ms);
