/*
 * SD card apps: scanning, tiles, loading and the dash_* API; see ext_apps.h
 * and sdk/dashboard_sdk/include/dashboard_app.h.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lvgl.h"
#include "dashboard_app.h"
#include "ext_apps.h"
#include "launcher.h"
#include "net_time.h"
#include "sd_card.h"
#include "settings.h"

static const char *TAG = "ext_apps";

#define APPS_DIR SD_DIR "/apps"

extern const struct esp_elfsym g_dash_app_symbols[];

typedef struct {
    ext_app_info_t info;
    char           symbol[8];
    uint32_t       color;
    app_t          tile;
} entry_t;

/* Shown list (LVGL task) and the scan's result (scan task -> LVGL task). */
static entry_t  s_list[EXT_MAX_APPS] EXT_RAM_BSS_ATTR;   /* PSRAM */
static int      s_n;
static entry_t  s_scan[EXT_MAX_APPS] EXT_RAM_BSS_ATTR;
static int      s_scan_n;
static bool     s_scan_ready, s_scanning, s_again;
static uint32_t s_version;              /* +1 when the shown list changes */

/* The loaded app. */
static esp_elf_t              s_elf;
static bool                   s_loaded;
static const dash_app_def_t  *s_def;
static lv_timer_t            *s_unload_timer;
static char                   s_missing[48];
/* An unsigned app waiting for "Run it". */
static uint8_t     *s_wait_buf;
static appk_info_t  s_wait_info;
static lv_obj_t    *s_wait_parent;

static void ext_create(lv_obj_t *parent);
static void ext_destroy(void);

/* ======================= dash_* API for apps ======================= */

void dash_app_register(const dash_app_def_t *def) { s_def = def; }
void dash_toast(const char *text, uint32_t ms)    { launcher_toast(text, ms); }
bool dash_time_is_set(void)                       { return net_time_is_synced(); }
bool dash_clock_24h(void)                         { return settings_get_24h(); }
void dash_free(void *p)                           { free(p); }

void *dash_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

int64_t dash_now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ======================= scanning ======================= */

static uint8_t *read_file(const char *path, size_t *len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > APPK_MAX_FILE) return NULL;
    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    size_t got = f ? fread(buf, 1, st.st_size, f) : 0;
    if (f) fclose(f);
    if (got != (size_t)st.st_size) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const entry_t *)a)->info.name, ((const entry_t *)b)->info.name);
}

static void scan_task(void *arg)
{
    (void)arg;
    int n = 0;
    DIR *dp = sd_card_mounted() ? opendir(APPS_DIR) : NULL;
    struct dirent *de;
    while (dp && (de = readdir(dp)) != NULL && n < EXT_MAX_APPS) {
        size_t l = strlen(de->d_name);
        if (l < 5 || l >= sizeof(s_scan[0].info.file) || strcasecmp(de->d_name + l - 4, ".app")) {
            continue;
        }
        entry_t *e = &s_scan[n++];
        memset(e, 0, sizeof(*e));
        snprintf(e->info.file, sizeof(e->info.file), "%.63s", de->d_name);
        char path[sizeof(APPS_DIR) + 72];
        snprintf(path, sizeof(path), APPS_DIR "/%.63s", de->d_name);
        size_t len;
        uint8_t *buf = read_file(path, &len);
        appk_info_t info;
        if (!buf) {
            snprintf(e->info.name, sizeof(e->info.name), "%.*s", (int)(l - 4 < 31 ? l - 4 : 31),
                     de->d_name);
            e->info.status = APPK_REFUSED;
            snprintf(e->info.why, sizeof(e->info.why), "couldn't be read (empty or over 2 MB)");
            continue;
        }
        e->info.status = app_keys_check(buf, len, &info, e->info.why, sizeof(e->info.why));
        free(buf);
        snprintf(e->info.name, sizeof(e->info.name), "%s", info.name);
        snprintf(e->info.version, sizeof(e->info.version), "%s", info.version);
        snprintf(e->info.signer, sizeof(e->info.signer), "%s", info.signer);
        e->info.is_signed = info.is_signed;
        snprintf(e->symbol, sizeof(e->symbol), "%s", info.symbol[0] ? info.symbol : LV_SYMBOL_PLAY);
        e->color = info.color ? info.color : 0x546E7A;
        ESP_LOGI(TAG, "%s: %s %s", de->d_name, info.name,
                 e->info.status == APPK_REFUSED ? e->info.why : "ok");
    }
    if (dp) closedir(dp);
    qsort(s_scan, n, sizeof(s_scan[0]), by_name);
    s_scan_n = n;
    __atomic_store_n(&s_scan_ready, true, __ATOMIC_RELEASE);
    __atomic_store_n(&s_scanning, false, __ATOMIC_RELEASE);
    vTaskDeleteWithCaps(NULL);           /* stack in PSRAM */
}

void ext_apps_rescan(void)
{
    bool expected = false;
    if (!__atomic_compare_exchange_n(&s_scanning, &expected, true, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return;                                 /* one is running; it'll do */
    }
    /* Not while the last result is unread (the scan writes into s_scan):
     * ext_apps_poll() starts it once that's taken. */
    if (__atomic_load_n(&s_scan_ready, __ATOMIC_ACQUIRE)) {
        s_again = true;
        __atomic_store_n(&s_scanning, false, __ATOMIC_RELEASE);
    } else if (xTaskCreateWithCaps(scan_task, "ext_apps", 6144, NULL, 2, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        __atomic_store_n(&s_scanning, false, __ATOMIC_RELEASE);
    }
}

bool ext_apps_poll(void)
{
    if (!__atomic_load_n(&s_scan_ready, __ATOMIC_ACQUIRE)) return false;
    bool changed = s_scan_n != s_n;
    for (int i = 0; !changed && i < s_n; i++) {
        changed = memcmp(&s_scan[i].info, &s_list[i].info, sizeof(s_list[i].info)) ||
                  strcmp(s_scan[i].symbol, s_list[i].symbol) || s_scan[i].color != s_list[i].color;
    }
    /* Don't swap the list under an open SD app. */
    if (changed && launcher_current_app() && launcher_current_app()->create == ext_create) {
        return false;
    }
    if (changed) {
        s_version++;
        memcpy(s_list, s_scan, sizeof(s_scan[0]) * s_scan_n);
        s_n = s_scan_n;
        for (int i = 0; i < s_n; i++) {
            s_list[i].tile = (app_t){
                .name = s_list[i].info.name, .symbol = s_list[i].symbol,
                .color_hex = s_list[i].color, .create = ext_create, .destroy = ext_destroy,
            };
        }
    }
    __atomic_store_n(&s_scan_ready, false, __ATOMIC_RELEASE);
    if (s_again) {
        s_again = false;
        ext_apps_rescan();
    }
    return changed;
}

int ext_apps_tiles(const app_t **out, int max)
{
    int k = 0;
    for (int i = 0; i < s_n && k < max; i++) {
        if (s_list[i].info.status != APPK_REFUSED) out[k++] = &s_list[i].tile;
    }
    return k;
}

int ext_apps_count(void) { return s_n; }
uint32_t ext_apps_version(void) { return s_version; }

bool ext_apps_get(int i, ext_app_info_t *out)
{
    if (i < 0 || i >= s_n) return false;
    *out = s_list[i].info;
    return true;
}

/* ======================= running ======================= */

static uintptr_t resolver(const char *name)
{
    uintptr_t a = elf_find_sym_default(name);
    if (!a && !s_missing[0]) snprintf(s_missing, sizeof(s_missing), "%s", name);
    return a;
}

static void unload_now(void)
{
    if (s_unload_timer) {
        lv_timer_del(s_unload_timer);
        s_unload_timer = NULL;
    }
    if (s_loaded) {
        esp_elf_deinit(&s_elf);
        s_loaded = false;
        ESP_LOGI(TAG, "App unloaded");
    }
    s_def = NULL;
}

static void unload_cb(lv_timer_t *t)
{
    (void)t;
    s_unload_timer = NULL;                      /* repeat count 1: LVGL deletes it */
    unload_now();
}

static void message(lv_obj_t *parent, const char *icon, const char *text)
{
    lv_obj_clean(parent);
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 18, 0);
    lv_obj_t *i = lv_label_create(box);
    lv_label_set_text(i, icon);
    lv_obj_set_style_text_font(i, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(i, lv_color_hex(0xFFB74D), 0);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, 680);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(APP_COLOR_TEXT), 0);
}

/* Loads the checked code (info->elf, inside buf, which is freed) and starts it. */
static void run(lv_obj_t *parent, uint8_t *buf, const appk_info_t *info)
{
    char text[160];
    unload_now();
    esp_elf_init(&s_elf);
    s_missing[0] = '\0';
    elf_set_symbol_resolver(resolver);
    int r = esp_elf_relocate(&s_elf, info->elf);    /* copies what it needs */
    elf_reset_symbol_resolver();
    free(buf);
    if (r != 0) {
        esp_elf_deinit(&s_elf);
        if (s_missing[0]) {
            snprintf(text, sizeof(text), "This app needs \"%s\", which this firmware doesn't "
                     "offer to apps. It may need a newer firmware.", s_missing);
        } else {
            snprintf(text, sizeof(text), "This app couldn't be loaded (error %d).", r);
        }
        message(parent, LV_SYMBOL_WARNING, text);
        return;
    }
    s_loaded = true;
    s_def = NULL;
    ESP_LOGI(TAG, "Starting %s %s", info->name, info->version);
    esp_elf_request(&s_elf, 0, 0, NULL);            /* app_main: registers the app */
    if (!s_def || !s_def->create || s_def->api_version != DASH_API_VERSION) {
        snprintf(text, sizeof(text), s_def && s_def->api_version != DASH_API_VERSION ?
                 "This app was made for a different version of the dashboard." :
                 "This app didn't start properly (it didn't register itself).");
        unload_now();
        message(parent, LV_SYMBOL_WARNING, text);
        return;
    }
    s_def->create(parent);
}

static void approve_async(void *arg)
{
    (void)arg;
    if (!s_wait_buf || !s_wait_parent) return;
    uint8_t *buf = s_wait_buf;
    s_wait_buf = NULL;
    if (!app_keys_approve(s_wait_info.sha)) {
        free(buf);
        message(s_wait_parent, LV_SYMBOL_WARNING, "Developer mode was turned off.");
        return;
    }
    lv_obj_clean(s_wait_parent);
    run(s_wait_parent, buf, &s_wait_info);
}

static void approve_clicked(lv_event_t *e)
{
    (void)e;
    lv_async_call(approve_async, NULL);     /* the button is deleted on the way */
}

static void ask_approval(lv_obj_t *parent, uint8_t *buf, const appk_info_t *info)
{
    char text[400], sha[20];
    snprintf(sha, sizeof(sha), "%02x%02x%02x%02x%02x%02x%02x%02x", info->sha[0], info->sha[1],
             info->sha[2], info->sha[3], info->sha[4], info->sha[5], info->sha[6], info->sha[7]);
    snprintf(text, sizeof(text), "\"%s\" isn't signed by a trusted key.\n\nIt will have full "
             "access to this display, including saved Wi-Fi passwords and keys. Run it only "
             "if you made it or trust whoever did.\n\nFile fingerprint %s...", info->name, sha);
    message(parent, LV_SYMBOL_WARNING, text);
    lv_obj_t *box = lv_obj_get_child(parent, 0);
    lv_obj_t *b = lv_btn_create(box);
    lv_obj_set_size(b, 240, 52);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x8E2A2A), 0);
    lv_obj_add_event_cb(b, approve_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, LV_SYMBOL_PLAY "  Run it");
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_center(l);
    s_wait_buf = buf;
    s_wait_info = *info;
    s_wait_parent = parent;
}

static void ext_create(lv_obj_t *parent)
{
    unload_now();
    const app_t *me = launcher_current_app();
    entry_t *e = NULL;
    for (int i = 0; i < s_n; i++) {
        if (&s_list[i].tile == me) e = &s_list[i];
    }
    if (!e) return;

    /* Read and check the file now, and run exactly these bytes. */
    char path[sizeof(APPS_DIR) + 72], why[96], text[200];
    snprintf(path, sizeof(path), APPS_DIR "/%s", e->info.file);
    size_t len;
    uint8_t *buf = read_file(path, &len);
    if (!buf) {
        message(parent, LV_SYMBOL_WARNING, "The app's file couldn't be read from the SD card.");
        return;
    }
    appk_info_t info;
    switch (app_keys_check(buf, len, &info, why, sizeof(why))) {
    case APPK_OK:
    case APPK_OK_DEV:
        run(parent, buf, &info);
        break;
    case APPK_NEEDS_APPROVAL:
        ask_approval(parent, buf, &info);
        break;
    default:
        free(buf);
        snprintf(text, sizeof(text), "This app won't run: %s.", why);
        message(parent, LV_SYMBOL_WARNING, text);
        break;
    }
}

static void ext_destroy(void)
{
    if (s_def && s_def->destroy) s_def->destroy();
    s_def = NULL;
    free(s_wait_buf);
    s_wait_buf = NULL;
    s_wait_parent = NULL;
    /* The launcher deletes the app's objects after this (asynchronously);
     * their callbacks may point into the app's code, so unload it later. */
    if (s_loaded && !s_unload_timer) {
        s_unload_timer = lv_timer_create(unload_cb, 1000, NULL);
        lv_timer_set_repeat_count(s_unload_timer, 1);
    }
}

void ext_apps_init(void)
{
    esp_elf_register_symbol(g_dash_app_symbols);
}
