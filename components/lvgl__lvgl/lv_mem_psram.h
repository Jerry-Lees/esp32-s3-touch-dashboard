#pragma once
/*
 * Project addition (not part of LVGL): LVGL's heap overflows to PSRAM.
 * Selected with CONFIG_LV_MEM_CUSTOM_INCLUDE="lv_mem_psram.h"; lv_mem.c
 * includes it after lv_conf_internal.h, so the macros below replace
 * malloc/realloc.
 *
 * Small allocations stay in internal RAM while more than LV_INTERNAL_KEEP
 * bytes of it would be left, then go to PSRAM. All-internal used up internal
 * RAM with a big screen (the Servers editor with 44 hosts: Wi-Fi then
 * aborted); all-PSRAM made the RGB screen flicker/jump, because rendering
 * then competes with the panel's DMA for PSRAM bandwidth. free() handles both.
 */
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#define LV_INTERNAL_KEEP   (80 * 1024)
#define LV_INTERNAL_MAX    1024          /* bigger blocks always go to PSRAM */

#define LV_CAPS_INT   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define LV_CAPS_EXT   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static inline int lv_internal_ok(size_t n)
{
    return n <= LV_INTERNAL_MAX && heap_caps_get_free_size(LV_CAPS_INT) > LV_INTERNAL_KEEP + n;
}

static inline void *lv_psram_alloc(size_t n)
{
    void *p = lv_internal_ok(n) ? heap_caps_malloc(n, LV_CAPS_INT) : NULL;
    if (!p) p = heap_caps_malloc(n, LV_CAPS_EXT);
    return p ? p : malloc(n);
}

/* lv_mem_realloc never asks for size 0 (it frees instead). */
static inline void *lv_psram_realloc(void *p, size_t n)
{
    void *q = NULL;
    if (p && esp_ptr_internal(p) && lv_internal_ok(n)) q = heap_caps_realloc(p, n, LV_CAPS_INT);
    if (!q) q = heap_caps_realloc(p, n, LV_CAPS_EXT);
    return q ? q : realloc(p, n);
}

#undef LV_MEM_CUSTOM_ALLOC
#undef LV_MEM_CUSTOM_REALLOC
#define LV_MEM_CUSTOM_ALLOC   lv_psram_alloc
#define LV_MEM_CUSTOM_REALLOC lv_psram_realloc
