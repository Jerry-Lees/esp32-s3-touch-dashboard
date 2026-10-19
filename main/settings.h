#pragma once
/*
 * User settings, saved in NVS (flash) so they survive a reboot.
 *
 * Call settings_init() once at startup, before anything reads a setting.
 * The getters return cached values and are cheap to call often.
 * The setters save to flash immediately.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void settings_init(void);

/* Clocks: true = 24-hour, false = 12-hour (default). */
bool settings_get_24h(void);
void settings_set_24h(bool on);

/* Backlight off after this many minutes without a touch; 0 = never (default). */
int  settings_get_screen_timeout_min(void);
void settings_set_screen_timeout_min(int minutes);

/* Show LVGL's FPS / CPU meter in the bottom-right corner; default off. */
bool settings_get_show_fps(void);
void settings_set_show_fps(bool on);

/* Index into the Weather app's location list; 0 = Lee's Summit (default). */
int  settings_get_weather_city(void);
void settings_set_weather_city(int index);

/* Small per-app values (0-255), e.g. a game's difficulty. Not cached:
 * each call reads or writes flash, so don't call them every frame.
 * Keys are at most 15 characters. */
int  settings_get_u8(const char *key, int def);
void settings_set_u8(const char *key, int value);
/* Same for larger numbers, e.g. a high score. */
int32_t settings_get_i32(const char *key, int32_t def);
void    settings_set_i32(const char *key, int32_t value);
/* Text (e.g. the St0nks watchlist). get copies the value into out (at most
 * len bytes, always terminated) and returns false if the key is missing. */
bool settings_get_str(const char *key, char *out, size_t len);
void settings_set_str(const char *key, const char *value);
