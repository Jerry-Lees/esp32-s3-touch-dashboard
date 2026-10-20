/*
 * Settings backup / restore. File format (version 1):
 *
 *   {
 *     "format": "esp32-dashboard-settings",
 *     "version": 1,
 *     "created": "2026-10-19 21:30:05",
 *     "settings": {
 *       "clock24":     { "type": "u8",  "value": 1 },
 *       "flappy_best": { "type": "i32", "value": 12 },
 *       "stonks_list": { "type": "str", "value": "GME,AMC,SPY" }
 *     }
 *   }
 *
 * Keys and values are whatever the apps saved with settings_set_*(). The
 * "wifi_rst" flag is device state, not a preference: never backed up, and
 * kept as it is on restore.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include "net_time.h"
#include "sd_card.h"
#include "settings_backup.h"

static const char *TAG = "backup";
static const char *NS = "settings";
static const char *FORMAT = "esp32-dashboard-settings";

#define FILE_MAX   16384
#define SKIP_KEY   "wifi_rst"

static const struct {
    nvs_type_t  type;
    const char *name;
} TYPES[] = {
    { NVS_TYPE_U8, "u8" },   { NVS_TYPE_I8, "i8" },   { NVS_TYPE_U16, "u16" },
    { NVS_TYPE_I16, "i16" }, { NVS_TYPE_U32, "u32" }, { NVS_TYPE_I32, "i32" },
    { NVS_TYPE_STR, "str" },
};

static const char *type_name(nvs_type_t t)
{
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (TYPES[i].type == t) return TYPES[i].name;
    }
    return NULL;
}

static bool type_from_name(const char *n, nvs_type_t *out)
{
    for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
        if (strcmp(TYPES[i].name, n) == 0) {
            *out = TYPES[i].type;
            return true;
        }
    }
    return false;
}

/* ---------- backup ---------- */

static cJSON *read_value(nvs_handle_t h, const char *key, nvs_type_t t)
{
    int64_t v = 0;
    esp_err_t err;
    switch (t) {
    case NVS_TYPE_U8:  { uint8_t x;  err = nvs_get_u8(h, key, &x);  v = x; break; }
    case NVS_TYPE_I8:  { int8_t x;   err = nvs_get_i8(h, key, &x);  v = x; break; }
    case NVS_TYPE_U16: { uint16_t x; err = nvs_get_u16(h, key, &x); v = x; break; }
    case NVS_TYPE_I16: { int16_t x;  err = nvs_get_i16(h, key, &x); v = x; break; }
    case NVS_TYPE_U32: { uint32_t x; err = nvs_get_u32(h, key, &x); v = x; break; }
    case NVS_TYPE_I32: { int32_t x;  err = nvs_get_i32(h, key, &x); v = x; break; }
    case NVS_TYPE_STR: {
        size_t len = 0;
        if (nvs_get_str(h, key, NULL, &len) != ESP_OK) return NULL;
        char *s = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!s) return NULL;
        cJSON *j = nvs_get_str(h, key, s, &len) == ESP_OK ? cJSON_CreateString(s) : NULL;
        free(s);
        return j;
    }
    default:
        return NULL;
    }
    return err == ESP_OK ? cJSON_CreateNumber((double)v) : NULL;
}

bool settings_backup_save(char *out, size_t len)
{
    if (!sd_card_mounted()) return false;

    char created[24] = "unknown", stamp[24] = "";
    if (net_time_is_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(created, sizeof(created), "%Y-%m-%d %H:%M:%S", &tm);
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", FORMAT);
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "created", created);
    cJSON *set = cJSON_AddObjectToObject(root, "settings");

    nvs_handle_t h;
    int count = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find_in_handle(h, NVS_TYPE_ANY, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            const char *tn = type_name(info.type);
            cJSON *v = tn && strcmp(info.key, SKIP_KEY) != 0
                       ? read_value(h, info.key, info.type) : NULL;
            if (v) {
                cJSON *e = cJSON_AddObjectToObject(set, info.key);
                cJSON_AddStringToObject(e, "type", tn);
                cJSON_AddItemToObject(e, "value", v);
                count++;
            } else if (strcmp(info.key, SKIP_KEY) != 0) {
                ESP_LOGW(TAG, "Skipping %s (unsupported type)", info.key);
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        nvs_close(h);
    }

    /* Pick a file name; without the time, the next free number. */
    char path[96];
    struct stat st;
    if (stamp[0]) {
        snprintf(path, sizeof(path), SD_DIR "/backup/settings_%s.json", stamp);
    } else {
        for (int i = 1; i < 10000; i++) {
            snprintf(path, sizeof(path), SD_DIR "/backup/settings_%04d.json", i);
            if (stat(path, &st) != 0) break;
        }
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    bool ok = false;
    FILE *f = text ? fopen(path, "w") : NULL;
    if (f) {
        size_t n = strlen(text);
        ok = fwrite(text, 1, n, f) == n;
        if (fclose(f) != 0) ok = false;
        if (!ok) remove(path);
    }
    free(text);
    if (ok) {
        strlcpy(out, path + strlen(SD_MOUNT_POINT "/"), len);
        ESP_LOGI(TAG, "Saved %d settings to %s", count, path);
    }
    return ok;
}

/* ---------- restore ---------- */

/* Checks one entry; on success fills *type. */
static bool check_entry(const cJSON *e, nvs_type_t *type, char *err, size_t errlen)
{
    const char *key = e->string;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "type");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "value");
    if (!key || !key[0] || strlen(key) > 15) {
        snprintf(err, errlen, "a setting name is empty or longer than 15 characters");
        return false;
    }
    if (!cJSON_IsString(t) || !type_from_name(t->valuestring, type)) {
        snprintf(err, errlen, "\"%s\" has an unknown type", key);
        return false;
    }
    if (*type == NVS_TYPE_STR ? !cJSON_IsString(v) : !cJSON_IsNumber(v)) {
        snprintf(err, errlen, "\"%s\" has a value of the wrong kind", key);
        return false;
    }
    if (*type == NVS_TYPE_STR && strlen(v->valuestring) > 1000) {
        snprintf(err, errlen, "\"%s\" is too long", key);
        return false;
    }
    return true;
}

static esp_err_t write_entry(nvs_handle_t h, const cJSON *e, nvs_type_t t)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(e, "value");
    const char *k = e->string;
    double d = v->valuedouble;
    switch (t) {
    case NVS_TYPE_U8:  return nvs_set_u8(h, k, (uint8_t)d);
    case NVS_TYPE_I8:  return nvs_set_i8(h, k, (int8_t)d);
    case NVS_TYPE_U16: return nvs_set_u16(h, k, (uint16_t)d);
    case NVS_TYPE_I16: return nvs_set_i16(h, k, (int16_t)d);
    case NVS_TYPE_U32: return nvs_set_u32(h, k, (uint32_t)d);
    case NVS_TYPE_I32: return nvs_set_i32(h, k, (int32_t)d);
    case NVS_TYPE_STR: return nvs_set_str(h, k, v->valuestring);
    default:           return ESP_FAIL;
    }
}

/* Applies a parsed backup: all entries are checked first, then the
 * "settings" namespace is cleared and refilled (keeping SKIP_KEY). */
static bool apply(const cJSON *root, int *count, char *err, size_t errlen)
{
    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *set = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (!cJSON_IsString(fmt) || strcmp(fmt->valuestring, FORMAT) != 0) {
        snprintf(err, errlen, "it isn't a settings backup from this display");
        return false;
    }
    if (!cJSON_IsNumber(ver) || ver->valueint != 1) {
        snprintf(err, errlen, "it's from a newer version of the firmware");
        return false;
    }
    if (!cJSON_IsObject(set)) {
        snprintf(err, errlen, "it has no \"settings\" section");
        return false;
    }
    const cJSON *e;
    nvs_type_t t;
    cJSON_ArrayForEach(e, set) {
        if (!check_entry(e, &t, err, errlen)) return false;
    }

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        snprintf(err, errlen, "the display's settings storage couldn't be opened");
        return false;
    }
    uint8_t keep = 0;
    bool had_keep = nvs_get_u8(h, SKIP_KEY, &keep) == ESP_OK;
    esp_err_t r = nvs_erase_all(h);
    if (had_keep && r == ESP_OK) r = nvs_set_u8(h, SKIP_KEY, keep);
    *count = 0;
    cJSON_ArrayForEach(e, set) {
        if (r != ESP_OK) break;
        if (strcmp(e->string, SKIP_KEY) == 0) continue;
        check_entry(e, &t, err, errlen);
        r = write_entry(h, e, t);
        (*count)++;
    }
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    if (r != ESP_OK) {
        snprintf(err, errlen, "saving failed (%s); some settings may be at defaults",
                 esp_err_to_name(r));
        return false;
    }
    return true;
}

bool settings_backup_import(void (*report)(const char *fmt, ...))
{
    DIR *dir = opendir(SD_DIR "/import");
    if (!dir) return false;
    char path[300] = "", name[256] = "";
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        size_t n = strlen(de->d_name);
        if (strncasecmp(de->d_name, "settings", 8) == 0 && n > 5 &&
            strcasecmp(de->d_name + n - 5, ".json") == 0) {
            strlcpy(name, de->d_name, sizeof(name));
            snprintf(path, sizeof(path), SD_DIR "/import/%s", de->d_name);
            break;
        }
    }
    closedir(dir);
    if (!path[0]) return false;

    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > FILE_MAX) {
        report("%s not restored: the file is empty or too big", name);
        return false;
    }
    char *text = heap_caps_calloc(1, st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!text) return false;
    FILE *f = fopen(path, "r");
    size_t got = f ? fread(text, 1, st.st_size, f) : 0;
    if (f) fclose(f);
    cJSON *root = got ? cJSON_Parse(text) : NULL;
    free(text);

    char err[120] = "it isn't valid JSON";
    int count = 0;
    bool ok = root && apply(root, &count, err, sizeof(err));
    cJSON_Delete(root);
    if (!ok) {
        report("%s not restored: %s", name, err);
        return false;
    }
    /* The inbox rule: applied files are removed (the original stays in
     * backup/). Settings aren't secret, so a plain delete is enough. */
    bool deleted = remove(path) == 0;
    report("Restored %d settings from %s%s. Restarting...", count, name,
           deleted ? "" : " (delete it from import/ by hand, or it will restore again)");
    return deleted;   /* restart only if it won't loop */
}
