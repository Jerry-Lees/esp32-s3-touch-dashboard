#pragma once
/*
 * Project addition (not part of LVGL): puts LVGL's heap in PSRAM. Selected
 * with CONFIG_LV_MEM_CUSTOM_INCLUDE="lv_mem_psram.h"; lv_mem.c includes it
 * after lv_conf_internal.h, so the macros below replace malloc/realloc.
 *
 * Without it every widget's small allocations (under
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) go to the scarce internal RAM, and a
 * screen with a few hundred widgets could use it all up (Wi-Fi then aborts).
 * Falls back to any memory if PSRAM is full; free() handles both.
 */
#include <stdlib.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

/* lv_mem_realloc never asks for size 0 (it frees instead). */
static inline void *lv_psram_realloc(void *p, size_t n)
{
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
}

#undef LV_MEM_CUSTOM_ALLOC
#undef LV_MEM_CUSTOM_REALLOC
#define LV_MEM_CUSTOM_ALLOC   lv_psram_alloc
#define LV_MEM_CUSTOM_REALLOC lv_psram_realloc
