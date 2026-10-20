/*
 * Wi-Fi connection (strongest known network) + SNTP internet time.
 *
 * Wi-Fi events arrive on the system event task, NOT the LVGL task, so this
 * file never touches LVGL. It only sets flags that the UI reads.
 */
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "net_time.h"
#include "rtc.h"
#include "settings.h"
#include "wifi_list.h"
/* Optional built-in network; without the file, networks come only from
 * the SD card import (wifi_list.c). */
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""
#endif

static const char *TAG = "net_time";

static volatile bool s_connected;
static bool s_started;
static volatile time_source_t s_source = TIME_SRC_NONE;
static volatile uint32_t s_last_sync;   /* fits until 2106; avoids torn 64-bit reads */

/* Runs in the SNTP (lwIP) task after each successful sync. */
static void sntp_synced(struct timeval *tv)
{
    s_last_sync = (uint32_t)tv->tv_sec;
    s_source = TIME_SRC_INTERNET;
    if (rtc_write_utc(tv->tv_sec)) {
        ESP_LOGI(TAG, "Internet time synced and saved to the RTC chip");
    }
}

static void restore_from_rtc(void)
{
    time_t t;
    if (!rtc_read_utc(&t)) {
        return;
    }
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_source = TIME_SRC_RTC;

    struct tm tm;
    char buf[32];
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "Time restored from the RTC chip: %s", buf);
}

/* ---------- Wi-Fi: connect to the strongest known network ----------
 *
 * Known networks = the saved list (wifi_list.c) plus the built-in one from
 * wifi_secrets.h. Everything below runs in the default event loop task, one
 * event at a time: other tasks ask for a (re)connect by posting NET_KICK
 * (net_time_networks_changed), and retries come from an esp_timer that
 * posts the same event.
 */

ESP_EVENT_DEFINE_BASE(NET_EVENT);
enum { NET_KICK };

#define MAX_SCAN 20

static bool    s_wifi_started;
static bool    s_scanning, s_connecting;            /* event task only */
static char    s_cur_ssid[WIFI_SSID_MAX + 1];       /* event task only */
static int     s_retry_ms = 2000;
static esp_timer_handle_t s_retry_timer;

static bool builtin_ok(void)
{
    return strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "your-network-name") != 0;
}

static bool is_known(const char *ssid)
{
    return wifi_list_contains(ssid) || (builtin_ok() && strcmp(ssid, WIFI_SSID) == 0);
}

static void kick(void)
{
    esp_event_post(NET_EVENT, NET_KICK, NULL, 0, 0);
}

static void retry_cb(void *arg)
{
    (void)arg;
    kick();
}

static void retry_later(int ms)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000);
}

static void start_scan(void)
{
    if (s_scanning || s_connecting || s_connected) return;
    if (!net_time_wifi_configured()) return;          /* nothing to look for */
    if (esp_wifi_scan_start(NULL, false) == ESP_OK) {
        s_scanning = true;
    } else {
        retry_later(5000);
    }
}

/* Picks the strongest known network from the scan and connects to it. */
static void scan_done(void)
{
    s_scanning = false;
    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *aps = calloc(MAX_SCAN, sizeof(*aps));
    if (!aps || esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
        esp_wifi_clear_ap_list();
        free(aps);
        retry_later(5000);
        return;
    }
    int best = -1;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (ssid[0] && is_known(ssid) && (best < 0 || aps[i].rssi > aps[best].rssi)) {
            best = i;
        }
    }
    if (best < 0) {
        free(aps);
        ESP_LOGI(TAG, "No known Wi-Fi network in range; trying again in 15 s");
        retry_later(15000);
        return;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, (const char *)aps[best].ssid, sizeof(cfg.sta.ssid));
    char *pass = (char *)cfg.sta.password;
    if (!wifi_list_password((const char *)cfg.sta.ssid, pass, sizeof(cfg.sta.password))) {
        strlcpy(pass, WIFI_PASSWORD, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Join the exact access point the scan found strongest (mesh systems
     * and extenders share one name), on its channel. */
    memcpy(cfg.sta.bssid, aps[best].bssid, sizeof(cfg.sta.bssid));
    cfg.sta.bssid_set = true;
    cfg.sta.channel = aps[best].primary;
    ESP_LOGI(TAG, "Connecting to Wi-Fi \"%s\" (%d dBm)...", cfg.sta.ssid, aps[best].rssi);
    strlcpy(s_cur_ssid, (const char *)cfg.sta.ssid, sizeof(s_cur_ssid));
    free(aps);

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) == ESP_OK && esp_wifi_connect() == ESP_OK) {
        s_connecting = true;
    } else {
        retry_later(5000);
    }
    memset(&cfg, 0, sizeof(cfg));                    /* don't leave the password around */
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        start_scan();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_done();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        bool was = s_connected;
        s_connected = false;
        s_connecting = false;
        ESP_LOGW(TAG, "Wi-Fi %s \"%s\" (reason %d); retrying in %d s",
                 was ? "disconnected from" : "couldn't connect to", s_cur_ssid,
                 ev ? ev->reason : -1, s_retry_ms / 1000);
        retry_later(s_retry_ms);
        s_retry_ms = s_retry_ms * 2 > 30000 ? 30000 : s_retry_ms * 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_connecting = false;
        s_retry_ms = 2000;
    } else if (base == NET_EVENT && id == NET_KICK) {
        /* A removed network must stop being used straight away. */
        if ((s_connected || s_connecting) && !is_known(s_cur_ssid)) {
            ESP_LOGI(TAG, "\"%s\" was removed; disconnecting", s_cur_ssid);
            esp_wifi_disconnect();
            return;
        }
        start_scan();
    }
}

static void wifi_start(void)
{
    esp_err_t err = esp_netif_init();
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

    /* The Wi-Fi driver normally keeps its own copy of the last network and
     * password in flash. Erase it once, and keep the driver's copy in RAM
     * only, so the saved list (wifi_list.c) is the only place passwords
     * live and removing a network really removes it. */
    if (!settings_get_u8("wifi_rst", 0)) {
        esp_wifi_restore();
        settings_set_u8("wifi_rst", 1);
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        NET_EVENT, NET_KICK, wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START then starts the first scan */
    s_wifi_started = true;
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

    /* Show the right time right away, even without Wi-Fi. */
    restore_from_rtc();

    /* Wi-Fi always starts, so networks imported later can be used without a
     * reboot. NVS is already initialized by settings_init(). */
    if (!net_time_wifi_configured()) {
        ESP_LOGW(TAG, "No Wi-Fi networks: import /dashboard/import/wifi.json from the "
                      "SD card or set main/wifi_secrets.h");
    }
    wifi_start();

    /* SNTP keeps retrying in the background until the network is up,
     * then re-syncs periodically (default: every hour). */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.sync_cb = sntp_synced;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_cfg));
}

void net_time_networks_changed(void)
{
    if (s_wifi_started) kick();
}

const char *net_time_builtin_ssid(void)
{
    return builtin_ok() ? WIFI_SSID : NULL;
}

bool net_time_wifi_configured(void) { return builtin_ok() || wifi_list_count() > 0; }
bool net_time_wifi_connected(void)  { return s_connected; }
time_source_t net_time_source(void)  { return s_source; }
time_t net_time_last_sync(void)      { return (time_t)s_last_sync; }

bool net_time_is_synced(void)
{
    /* Before RTC restore or sync the clock starts at 1970; anything after
     * 2025 is real. */
    return time(NULL) > 1735689600; /* 2025-01-01 00:00:00 UTC */
}
