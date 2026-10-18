#pragma once
/*
 * User settings, saved in NVS (flash) so they survive a reboot.
 *
 * Call settings_init() once at startup, before anything reads a setting.
 * The getters return cached values and are cheap to call often.
 * The setters save to flash immediately.
 */
#include <stdbool.h>

void settings_init(void);

/* Clocks: true = 24-hour, false = 12-hour (default). */
bool settings_get_24h(void);
void settings_set_24h(bool on);

/* Backlight off after this many minutes without a touch; 0 = never (default). */
int  settings_get_screen_timeout_min(void);
void settings_set_screen_timeout_min(int minutes);

/* Index into the Weather app's location list; 0 = Lee's Summit (default). */
int  settings_get_weather_city(void);
void settings_set_weather_city(int index);
