#pragma once
/*
 * The dashboard's fonts and colours, shared by the firmware's apps and SD
 * card apps so they look alike.
 */
#include "lvgl.h"

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
