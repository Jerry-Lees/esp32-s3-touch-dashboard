/*
 * Saved Wi-Fi networks: one NVS blob ("wifi"/"nets") holding an array of
 * wifi_net_t, cached in RAM behind a mutex.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

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

uint32_t wifi_list_version(void)
{
    return __atomic_load_n(&s_version, __ATOMIC_ACQUIRE);
}
