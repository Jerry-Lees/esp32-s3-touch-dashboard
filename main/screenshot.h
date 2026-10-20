#pragma once
/*
 * Screenshots: long-press `obj` (the launcher's top bars). With an SD card
 * in, the screen is saved as dashboard/screenshots/shot_<date_time>.bmp;
 * without one, it's sent over the USB serial console as base64 text, which
 * tools/screenshot.py on the PC turns into PNG files. LVGL task only.
 */
#include "lvgl.h"

void screenshot_attach(lv_obj_t *obj);
