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
#include "esp_memory_utils.h"

#include "lvgl.h"
#include "dashboard_app.h"
#include "ext_apps.h"
#include "game_common.h"
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
    char           folder[16];
    uint8_t       *icon;            /* PSRAM copy of the tile picture, or NULL */
    uint16_t       icon_w, icon_h;
    lv_img_dsc_t   icon_dsc;
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
static char                   s_cur_name[APPK_NAME_MAX];  /* running app, for dash_keep */
static uint8_t                s_cur_key[8];
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

/* Apps' numbers live in the settings namespace (so backups include them)
 * under "x" + key, apart from the firmware's own keys. */
static bool app_key(const char *key, char out[16])
{
    size_t n = key ? strnlen(key, 15) : 0;
    if (n < 1 || n > 14) return false;
    for (size_t i = 0; i < n; i++) {
        if (key[i] <= ' ' || key[i] > '~') return false;
    }
    out[0] = 'x';
    memcpy(out + 1, key, n + 1);
    return true;
}

/* NVS needs a stack in internal RAM (flash access turns PSRAM off). */
static bool nvs_safe_here(const char *what)
{
    int probe = 0;
    if (esp_ptr_external_ram(&probe)) {
        ESP_LOGW(TAG, "%s: only from the LVGL task (create, events, timers)", what);
        return false;
    }
    return true;
}

/* Where the games kept these before they moved to the SD card: read once
 * so best scores and levels carry over. */
static const struct { const char *key; bool u8; } LEGACY[] = {
    { "chess_lvl", true }, { "checkers_lvl", true },
    { "flappy_best", false }, { "snake_best", false }, { "2048_best", false },
};

int32_t dash_get_int(const char *key, int32_t def)
{
    char k[16];
    if (!app_key(key, k) || !nvs_safe_here("dash_get_int")) return def;
    int32_t v = settings_get_i32(k, INT32_MIN);
    if (v != INT32_MIN) return v;
    for (size_t i = 0; i < sizeof(LEGACY) / sizeof(LEGACY[0]); i++) {
        if (strcmp(key, LEGACY[i].key)) continue;
        if (LEGACY[i].u8) {
            int u = settings_get_u8(key, -1);
            return u < 0 ? def : u;
        }
        return settings_get_i32(key, def);
    }
    return def;
}

void dash_set_int(const char *key, int32_t value)
{
    char k[16];
    if (app_key(key, k) && nvs_safe_here("dash_set_int")) settings_set_i32(k, value);
}

/* dash_keep blocks: owned by app (name + signer), never freed. */
#define KEEP_MAX      32
#define KEEP_PER_APP  8
#define KEEP_BLOCK    (256 * 1024)
#define KEEP_TOTAL    (1024 * 1024)

typedef struct {
    char    app[APPK_NAME_MAX];
    uint8_t key[8];
    char    name[24];
    size_t  size;
    void   *p;
} keep_t;
static keep_t s_keep[KEEP_MAX] EXT_RAM_BSS_ATTR;
static size_t s_keep_total;

void *dash_keep(const char *name, size_t size)
{
    if (!s_loaded || !name || !name[0] || strlen(name) >= sizeof(s_keep[0].name) ||
        size == 0 || size > KEEP_BLOCK) {
        return NULL;
    }
    int mine = 0;
    keep_t *free_slot = NULL;
    for (int i = 0; i < KEEP_MAX; i++) {
        keep_t *k = &s_keep[i];
        if (!k->p) {
            if (!free_slot) free_slot = k;
            continue;
        }
        if (strcmp(k->app, s_cur_name) || memcmp(k->key, s_cur_key, 8)) continue;
        if (strcmp(k->name, name)) {
            mine++;
            continue;
        }
        if (k->size == size) return k->p;
        /* Same name, new size (a new version of the app): start over. */
        free(k->p);
        s_keep_total -= k->size;
        memset(k, 0, sizeof(*k));
        if (!free_slot || free_slot > k) free_slot = k;
    }
    if (!free_slot || mine >= KEEP_PER_APP || s_keep_total + size > KEEP_TOTAL) return NULL;
    void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return NULL;
    snprintf(free_slot->app, sizeof(free_slot->app), "%s", s_cur_name);
    memcpy(free_slot->key, s_cur_key, 8);
    snprintf(free_slot->name, sizeof(free_slot->name), "%s", name);
    free_slot->size = size;
    free_slot->p = p;
    s_keep_total += size;
    return p;
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
        if (e->info.status != APPK_REFUSED && info.icon) {
            size_t isz = (size_t)info.icon_w * info.icon_h * 3;
            e->icon = heap_caps_malloc(isz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (e->icon) {
                memcpy(e->icon, info.icon, isz);
                e->icon_w = info.icon_w;
                e->icon_h = info.icon_h;
            }
        }
        snprintf(e->folder, sizeof(e->folder), "%s", info.folder);
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

static bool same_icon(const entry_t *a, const entry_t *b)
{
    if (a->icon_w != b->icon_w || a->icon_h != b->icon_h || !a->icon != !b->icon) return false;
    return !a->icon || !memcmp(a->icon, b->icon, (size_t)a->icon_w * a->icon_h * 3);
}

bool ext_apps_poll(void)
{
    if (!__atomic_load_n(&s_scan_ready, __ATOMIC_ACQUIRE)) return false;
    bool changed = s_scan_n != s_n;
    for (int i = 0; !changed && i < s_n; i++) {
        changed = memcmp(&s_scan[i].info, &s_list[i].info, sizeof(s_list[i].info)) ||
                  strcmp(s_scan[i].symbol, s_list[i].symbol) || s_scan[i].color != s_list[i].color ||
                  strcmp(s_scan[i].folder, s_list[i].folder) || !same_icon(&s_scan[i], &s_list[i]);
    }
    /* Don't swap the list under an open SD app. */
    if (changed && launcher_current_app() && launcher_current_app()->create == ext_create) {
        return false;
    }
    if (changed) {
        /* The caller rebuilds the tiles right away, before anything is drawn. */
        for (int i = 0; i < s_n; i++) free(s_list[i].icon);
        lv_img_cache_invalidate_src(NULL);
        s_version++;
        memcpy(s_list, s_scan, sizeof(s_scan[0]) * s_scan_n);
        s_n = s_scan_n;
        for (int i = 0; i < s_n; i++) {
            entry_t *e = &s_list[i];
            e->icon_dsc = (lv_img_dsc_t){
                .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .header.w = e->icon_w,
                .header.h = e->icon_h, .data_size = (uint32_t)e->icon_w * e->icon_h * 3,
                .data = e->icon,
            };
            e->tile = (app_t){
                .name = e->info.name, .symbol = e->symbol,
                .color_hex = e->color, .create = ext_create, .destroy = ext_destroy,
                .icon_img = e->icon ? &e->icon_dsc : NULL,
            };
        }
    } else {
        for (int i = 0; i < s_scan_n; i++) free(s_scan[i].icon);
    }
    s_scan_n = 0;
    __atomic_store_n(&s_scan_ready, false, __ATOMIC_RELEASE);
    if (s_again) {
        s_again = false;
        ext_apps_rescan();
    }
    return changed;
}

int ext_apps_tiles(const app_t **out, int max, bool games)
{
    int k = 0;
    for (int i = 0; i < s_n && k < max; i++) {
        bool g = !strcasecmp(s_list[i].folder, "Games");
        if (s_list[i].info.status != APPK_REFUSED && g == games) out[k++] = &s_list[i].tile;
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
    /* A game's background search runs the app's code: it was told to stop
     * in destroy() and normally ends within a few ms. */
    for (int i = 0; s_loaded && game_worker_busy() && i < 300; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_loaded && game_worker_busy()) {
        ESP_LOGE(TAG, "The app's background job didn't stop; keeping its code loaded");
        s_loaded = false;               /* leaked on purpose: unloading would crash */
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
    if (game_worker_busy()) return;             /* try again in a second */
    lv_timer_del(t);
    s_unload_timer = NULL;
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
    snprintf(s_cur_name, sizeof(s_cur_name), "%s", info->name);
    memcpy(s_cur_key, info->is_signed ? info->key_id : (const uint8_t[8]){0}, 8);
    ESP_LOGI(TAG, "Starting %s %s", info->name, info->version);
    esp_elf_request(&s_elf, 0, 0, NULL);            /* app_main: registers the app */
    bool version_ok = s_def && s_def->api_version >= 1 && s_def->api_version <= DASH_API_VERSION;
    if (!s_def || !s_def->create || !version_ok) {
        snprintf(text, sizeof(text), s_def && !version_ok ?
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
    }
}

void ext_apps_init(void)
{
    esp_elf_register_symbol(g_dash_app_symbols);
}
