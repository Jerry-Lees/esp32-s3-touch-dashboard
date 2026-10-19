#pragma once
/*
 * Screenshots for the README: long-press `obj` (the launcher's top bars) and
 * the current screen is sent over the USB serial console as base64 text.
 * tools/screenshot.py on the PC turns it back into PNG files. LVGL task only.
 */
#include "lvgl.h"

void screenshot_attach(lv_obj_t *obj);
