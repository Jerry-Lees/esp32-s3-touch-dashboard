#pragma once
/*
 * Starts Wi-Fi/time sync and shows the app launcher (home screen).
 * Call it once, while holding the LVGL lock, in place of lv_demo_music().
 */
void launcher_start(void);

/* Shows or hides LVGL's FPS / CPU meter to match settings_get_show_fps(). */
void launcher_apply_fps(void);
