/*
 * User settings stored in NVS under the "settings" namespace.
 * Each value is one uint8_t key; a missing key means "use the default".
 */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"

static const char *TAG = "settings";
static const char *NS  = "settings";

static bool s_24h;
static int  s_timeout_min;
static int  s_weather_city;

static uint8_t load_u8(nvs_handle_t h, const char *key, uint8_t def)
{
    uint8_t v = def;
    nvs_get_u8(h, key, &v);   /* leaves v untouched if the key is missing */
    return v;
}

static void save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, key, v);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving %s failed: %s", key, esp_err_to_name(err));
    }
}

void settings_init(void)
{
    /* Safe to call again later (net_time_start() does). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        s_24h          = load_u8(h, "clock24", 0) != 0;
        s_timeout_min  = load_u8(h, "scr_tmo", 0);
        s_weather_city = load_u8(h, "wx_city", 0);
        nvs_close(h);
    }
    /* nvs_open fails on first boot (namespace not created yet): keep defaults. */

    ESP_LOGI(TAG, "24h=%d timeout=%d min weather_city=%d",
             s_24h, s_timeout_min, s_weather_city);
}

bool settings_get_24h(void) { return s_24h; }

void settings_set_24h(bool on)
{
    if (on != s_24h) {
        s_24h = on;
        save_u8("clock24", on);
    }
}

int settings_get_screen_timeout_min(void) { return s_timeout_min; }

void settings_set_screen_timeout_min(int minutes)
{
    if (minutes < 0) minutes = 0;
    if (minutes > 255) minutes = 255;
    if (minutes != s_timeout_min) {
        s_timeout_min = minutes;
        save_u8("scr_tmo", (uint8_t)minutes);
    }
}

int settings_get_u8(const char *key, int def)
{
    int v = def;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        v = load_u8(h, key, (uint8_t)def);
        nvs_close(h);
    }
    return v;
}

void settings_set_u8(const char *key, int value)
{
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    if (settings_get_u8(key, -1) != value) {
        save_u8(key, (uint8_t)value);
    }
}

int settings_get_weather_city(void) { return s_weather_city; }

void settings_set_weather_city(int index)
{
    if (index < 0 || index > 255) index = 0;
    if (index != s_weather_city) {
        s_weather_city = index;
        save_u8("wx_city", (uint8_t)index);
    }
}
