#pragma once
#include <stdbool.h>

/* POSIX time-zone rules (daylight saving handled automatically). */
#define TZ_US_CENTRAL "CST6CDT,M3.2.0,M11.1.0"
#define TZ_US_EASTERN "EST5EDT,M3.2.0,M11.1.0"
#define TZ_LONDON     "GMT0BST,M3.5.0/1,M10.5.0"
#define TZ_UTC        "UTC0"

/* The device's own time zone. */
#define LOCAL_TZ      TZ_US_CENTRAL

/* Connects to Wi-Fi (from wifi_secrets.h) and starts internet time sync. */
void net_time_start(void);

bool net_time_wifi_configured(void);
bool net_time_wifi_connected(void);
bool net_time_is_synced(void);
