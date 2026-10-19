#pragma once
/*
 * Info ticker: a strip along the bottom of the home screen with scrolling
 * weather and stock updates (sources and their settings: see
 * app_framework.h and Settings). LVGL task only.
 */
#include <stdbool.h>
#include "lvgl.h"

#define TICKER_H 96   /* 20% of the 480 px screen */

/* Creates the strip at the bottom of `parent`. Only one exists at a time;
 * deleting the object (or its parent) removes it. */
lv_obj_t *ticker_create(lv_obj_t *parent);

/* Scrolls only while running (the launcher pauses it when the home screen
 * isn't showing or the backlight is off). */
void ticker_set_running(bool on);

/* Keeps the ticker's data fresh in the background. Call about once a
 * second whether or not the strip is visible. */
void ticker_service(void);
