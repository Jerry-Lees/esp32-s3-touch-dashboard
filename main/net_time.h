#pragma once
#include <stdbool.h>
#include <time.h>

/* POSIX time-zone rules (daylight saving handled automatically). */
#define TZ_US_CENTRAL "CST6CDT,M3.2.0,M11.1.0"
#define TZ_US_EASTERN "EST5EDT,M3.2.0,M11.1.0"
#define TZ_LONDON     "GMT0BST,M3.5.0/1,M10.5.0"
#define TZ_UTC        "UTC0"

/* The device's own time zone. */
#define LOCAL_TZ      TZ_US_CENTRAL

/* Restores the time from the RTC chip (if it has a valid time), then
 * starts Wi-Fi and internet time sync. Wi-Fi connects to the strongest
 * known network: the saved list (wifi_list.h) or the built-in one from
 * wifi_secrets.h. Every internet sync is also saved to the RTC chip.
 * Call after settings_init() and wifi_list_init(). */
void net_time_start(void);

/* Call after adding or removing saved networks (any task): connects if
 * not connected yet, and drops the connection if its network was removed. */
void net_time_networks_changed(void);

/* The network name from wifi_secrets.h, or NULL if it isn't set. */
const char *net_time_builtin_ssid(void);

/* Where the current time came from. */
typedef enum {
    TIME_SRC_NONE,      /* not set yet */
    TIME_SRC_RTC,       /* restored from the RTC chip at boot */
    TIME_SRC_INTERNET,  /* synced from pool.ntp.org since boot */
} time_source_t;

time_source_t net_time_source(void);
time_t        net_time_last_sync(void);   /* last internet sync, 0 = never */

/* True if any network is known (saved or built-in). */
bool net_time_wifi_configured(void);
bool net_time_wifi_connected(void);
/* True once the clock holds a real time (from the RTC or the internet). */
bool net_time_is_synced(void);
