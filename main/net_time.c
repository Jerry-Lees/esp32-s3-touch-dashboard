/*
 * Wi-Fi connection + SNTP internet time.
 *
 * Wi-Fi events arrive on the system event task, NOT the LVGL task, so this
 * file never touches LVGL. It only sets flags that the UI reads.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "net_time.h"
#include "wifi_secrets.h"

static const char *TAG = "net_time";

static volatile bool s_configured;
static volatile bool s_connected;
static bool s_started;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
    }
}

void net_time_start(void)
{
    if (s_started) {
        return;
    }
    s_started = true;

    /* Local time zone for anything that calls localtime(). */
    setenv("TZ", LOCAL_TZ, 1);
    tzset();

    if (strlen(WIFI_SSID) == 0 || strcmp(WIFI_SSID, "your-network-name") == 0) {
        ESP_LOGW(TAG, "Wi-Fi not configured: edit main/wifi_secrets.h");
        return;
    }
    s_configured = true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode =
        strlen(WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* SNTP keeps retrying in the background until the network is up,
     * then re-syncs periodically (default: every hour). */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));

    ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\"...", WIFI_SSID);
}

bool net_time_wifi_configured(void) { return s_configured; }
bool net_time_wifi_connected(void)  { return s_connected; }

bool net_time_is_synced(void)
{
    /* Before sync the clock starts at 1970; anything after 2025 is real. */
    return time(NULL) > 1735689600; /* 2025-01-01 00:00:00 UTC */
}
