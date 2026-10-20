/*
 * Saved Wi-Fi networks: one NVS blob ("wifi"/"nets") holding an array of
 * wifi_net_t, cached in RAM behind a mutex.
 */
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi_list.h"

static const char *TAG = "wifi_list";

static SemaphoreHandle_t s_lock;
static wifi_net_t        s_nets[WIFI_LIST_MAX];
static int               s_count;
static uint32_t          s_version;   /* __atomic */

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static int find(const char *ssid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

/* Writes nets[0..n) to NVS (an empty list erases the key). */
static bool save(const wifi_net_t *nets, int n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = n > 0 ? nvs_set_blob(h, "nets", nets, sizeof(wifi_net_t) * n)
                    : nvs_erase_key(h, "nets");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the network list failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void wifi_list_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t size = sizeof(s_nets);
        if (nvs_get_blob(h, "nets", s_nets, &size) == ESP_OK) {
            s_count = size / sizeof(wifi_net_t);
        }
        nvs_close(h);
    }
    for (int i = 0; i < s_count; i++) {          /* never trust stored data */
        s_nets[i].ssid[WIFI_SSID_MAX] = '\0';
        s_nets[i].pass[WIFI_PASS_MAX] = '\0';
    }
    ESP_LOGI(TAG, "%d saved network(s)", s_count);
}

int wifi_list_count(void)
{
    lock();
    int n = s_count;
    unlock();
    return n;
}

bool wifi_list_ssid(int i, char *out, size_t len)
{
    lock();
    bool ok = i >= 0 && i < s_count;
    if (ok) strlcpy(out, s_nets[i].ssid, len);
    unlock();
    return ok;
}

bool wifi_list_contains(const char *ssid)
{
    lock();
    bool ok = find(ssid) >= 0;
    unlock();
    return ok;
}

bool wifi_list_password(const char *ssid, char *out, size_t len)
{
    lock();
    int i = find(ssid);
    if (i >= 0) strlcpy(out, s_nets[i].pass, len);
    unlock();
    return i >= 0;
}

bool wifi_list_add(const wifi_net_t *nets, int n)
{
    static wifi_net_t next[WIFI_LIST_MAX];       /* under the lock */
    lock();
    memcpy(next, s_nets, sizeof(next));
    int count = s_count;
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        int j = -1;
        for (int k = 0; k < count; k++) {
            if (strcmp(next[k].ssid, nets[i].ssid) == 0) j = k;
        }
        if (j < 0) {
            if (count >= WIFI_LIST_MAX) {
                ok = false;
                break;
            }
            j = count++;
        }
        memset(&next[j], 0, sizeof(next[j]));
        strlcpy(next[j].ssid, nets[i].ssid, sizeof(next[j].ssid));
        strlcpy(next[j].pass, nets[i].pass, sizeof(next[j].pass));
    }
    if (ok && save(next, count)) {
        memcpy(s_nets, next, sizeof(s_nets));
        s_count = count;
        __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
    } else {
        ok = false;
    }
    memset(next, 0, sizeof(next));               /* don't leave passwords around */
    unlock();
    return ok;
}

bool wifi_list_remove(const char *ssid)
{
    lock();
    int i = find(ssid);
    bool ok = i >= 0;
    if (ok) {
        static wifi_net_t next[WIFI_LIST_MAX];
        memcpy(next, s_nets, sizeof(next));
        memmove(&next[i], &next[i + 1], sizeof(wifi_net_t) * (s_count - i - 1));
        memset(&next[s_count - 1], 0, sizeof(wifi_net_t));
        ok = save(next, s_count - 1);
        if (ok) {
            memcpy(s_nets, next, sizeof(s_nets));
            s_count--;
            __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
        }
        memset(next, 0, sizeof(next));
    }
    unlock();
    return ok;
}

/* ---------- secure wipe ---------- */

#define KEEP_MAX 128   /* NVS values we can carry over (we have a few dozen) */

typedef struct {
    char        ns[NVS_NS_NAME_MAX_SIZE];
    char        key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t  type;
    size_t      len;
    void       *data;        /* PSRAM */
} kept_t;

static bool skip_namespace(const char *ns)
{
    /* Ours, and the Wi-Fi driver's own credential store. */
    return strcmp(ns, "wifi") == 0 || strcmp(ns, "nvs.net80211") == 0;
}

static size_t int_size(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_U8:  case NVS_TYPE_I8:  return 1;
    case NVS_TYPE_U16: case NVS_TYPE_I16: return 2;
    case NVS_TYPE_U32: case NVS_TYPE_I32: return 4;
    case NVS_TYPE_U64: case NVS_TYPE_I64: return 8;
    default: return 0;
    }
}

static bool read_value(kept_t *k)
{
    nvs_handle_t h;
    if (nvs_open(k->ns, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = ESP_FAIL;
    uint64_t num = 0;
    switch (k->type) {
    case NVS_TYPE_U8:  err = nvs_get_u8(h, k->key, (uint8_t *)&num); break;
    case NVS_TYPE_I8:  err = nvs_get_i8(h, k->key, (int8_t *)&num); break;
    case NVS_TYPE_U16: err = nvs_get_u16(h, k->key, (uint16_t *)&num); break;
    case NVS_TYPE_I16: err = nvs_get_i16(h, k->key, (int16_t *)&num); break;
    case NVS_TYPE_U32: err = nvs_get_u32(h, k->key, (uint32_t *)&num); break;
    case NVS_TYPE_I32: err = nvs_get_i32(h, k->key, (int32_t *)&num); break;
    case NVS_TYPE_U64: err = nvs_get_u64(h, k->key, &num); break;
    case NVS_TYPE_I64: err = nvs_get_i64(h, k->key, (int64_t *)&num); break;
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB:
        err = k->type == NVS_TYPE_STR ? nvs_get_str(h, k->key, NULL, &k->len)
                                      : nvs_get_blob(h, k->key, NULL, &k->len);
        if (err != ESP_OK) break;
        k->data = heap_caps_malloc(k->len ? k->len : 1, MALLOC_CAP_SPIRAM);
        if (!k->data) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        err = k->type == NVS_TYPE_STR ? nvs_get_str(h, k->key, k->data, &k->len)
                                      : nvs_get_blob(h, k->key, k->data, &k->len);
        break;
    default:
        break;
    }
    if (err == ESP_OK && int_size(k->type)) {
        k->len = int_size(k->type);
        k->data = heap_caps_malloc(8, MALLOC_CAP_SPIRAM);
        if (k->data) memcpy(k->data, &num, 8);   /* little-endian: low bytes first */
        else err = ESP_ERR_NO_MEM;
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool write_value(const kept_t *k)
{
    nvs_handle_t h;
    if (nvs_open(k->ns, NVS_READWRITE, &h) != ESP_OK) return false;
    uint64_t num = 0;
    if (int_size(k->type)) memcpy(&num, k->data, 8);
    esp_err_t err;
    switch (k->type) {
    case NVS_TYPE_U8:   err = nvs_set_u8(h, k->key, (uint8_t)num); break;
    case NVS_TYPE_I8:   err = nvs_set_i8(h, k->key, (int8_t)num); break;
    case NVS_TYPE_U16:  err = nvs_set_u16(h, k->key, (uint16_t)num); break;
    case NVS_TYPE_I16:  err = nvs_set_i16(h, k->key, (int16_t)num); break;
    case NVS_TYPE_U32:  err = nvs_set_u32(h, k->key, (uint32_t)num); break;
    case NVS_TYPE_I32:  err = nvs_set_i32(h, k->key, (int32_t)num); break;
    case NVS_TYPE_U64:  err = nvs_set_u64(h, k->key, num); break;
    case NVS_TYPE_I64:  err = nvs_set_i64(h, k->key, (int64_t)num); break;
    case NVS_TYPE_STR:  err = nvs_set_str(h, k->key, k->data); break;
    case NVS_TYPE_BLOB: err = nvs_set_blob(h, k->key, k->data, k->len); break;
    default:            err = ESP_FAIL; break;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool wifi_list_wipe(void)
{
    kept_t *keep = heap_caps_calloc(KEEP_MAX, sizeof(kept_t), MALLOC_CAP_SPIRAM);
    if (!keep) return false;
    lock();

    /* 1. Copy every value we want to keep into RAM. */
    int n = 0;
    bool ok = true;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (!skip_namespace(info.namespace_name)) {
            if (n >= KEEP_MAX) {
                ok = false;
                break;
            }
            strlcpy(keep[n].ns, info.namespace_name, sizeof(keep[n].ns));
            strlcpy(keep[n].key, info.key, sizeof(keep[n].key));
            keep[n].type = info.type;
            n++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (int i = 0; i < n && ok; i++) {
        if (!read_value(&keep[i])) {
            ESP_LOGE(TAG, "Couldn't read %s/%s", keep[i].ns, keep[i].key);
            ok = false;
        }
    }

    /* 2. Erase the whole partition (a real flash erase), 3. write back. */
    if (ok) {
        nvs_flash_deinit();
        ok = nvs_flash_erase() == ESP_OK && nvs_flash_init() == ESP_OK;
        int restored = 0;
        for (int i = 0; ok && i < n; i++) restored += write_value(&keep[i]);
        if (ok && restored != n) {
            ESP_LOGE(TAG, "Only %d of %d settings could be restored", restored, n);
            ok = false;
        }
        memset(s_nets, 0, sizeof(s_nets));
        s_count = 0;
        __atomic_add_fetch(&s_version, 1, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Wiped saved networks; kept %d other settings", n);
    } else {
        ESP_LOGE(TAG, "Wipe cancelled: couldn't copy the other settings; nothing erased");
    }

    unlock();
    for (int i = 0; i < KEEP_MAX; i++) free(keep[i].data);
    free(keep);
    return ok;
}

uint32_t wifi_list_version(void)
{
    return __atomic_load_n(&s_version, __ATOMIC_ACQUIRE);
}
