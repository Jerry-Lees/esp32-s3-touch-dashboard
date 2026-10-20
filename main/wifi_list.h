#pragma once
/*
 * Saved Wi-Fi networks (up to WIFI_LIST_MAX), kept in NVS (namespace "wifi").
 * Added by importing /dashboard/import/wifi.json from the SD card; removed
 * in Settings > Network. The network in wifi_secrets.h is a built-in
 * fallback and isn't part of this list.
 *
 * Thread-safe: used by the SD card task, the Wi-Fi event task and the LVGL
 * task. Passwords never leave this module except to net_time.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_LIST_MAX   10
#define WIFI_SSID_MAX   32   /* bytes, as in 802.11 */
#define WIFI_PASS_MAX   64

/* Loads the list from NVS. Call once after NVS is initialized. */
void wifi_list_init(void);

int  wifi_list_count(void);
/* Copies network i's name into out; false if i is out of range. */
bool wifi_list_ssid(int i, char *out, size_t len);
bool wifi_list_contains(const char *ssid);

/* For net_time.c only: copies the saved password for ssid; false if the
 * network isn't saved. */
bool wifi_list_password(const char *ssid, char *out, size_t len);

/* Adds networks in one go, replacing the password of any already saved.
 * All or nothing: returns false (and changes nothing) if the result would
 * have more than WIFI_LIST_MAX networks or saving fails. */
typedef struct {
    char ssid[WIFI_SSID_MAX + 1];
    char pass[WIFI_PASS_MAX + 1];
} wifi_net_t;
bool wifi_list_add(const wifi_net_t *nets, int n);

/* Removes a saved network; false if it wasn't saved. */
bool wifi_list_remove(const char *ssid);

/* Securely wipes every saved network, including leftovers of removed ones
 * that NVS only marked as deleted: copies all other NVS values to RAM,
 * erases the whole NVS partition, then writes them back. LVGL task only
 * (the other NVS writers live there). Returns false if something failed:
 * if copying failed nothing is erased; if restoring failed some settings
 * may be back at their defaults. */
bool wifi_list_wipe(void);

/* Increases on every change, so the UI can tell when to redraw. */
uint32_t wifi_list_version(void);
