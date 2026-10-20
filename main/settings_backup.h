#pragma once
/*
 * Settings backup and restore on the SD card. A backup is a JSON copy of
 * the NVS "settings" namespace (every app's saved values), never the Wi-Fi
 * networks. Restore = put a backup in dashboard/import/ (any name
 * settings*.json) and restart: it's applied, deleted, and the display
 * restarts so every app reloads its values.
 */
#include <stdbool.h>
#include <stddef.h>

/* Writes dashboard/backup/settings_<date_time>.json. On success copies the
 * path (relative to the card) into out. LVGL task. */
bool settings_backup_save(char *out, size_t len);

/* Looks for dashboard/import/settings*.json and restores it. Called by the
 * SD card task at startup; reports through `report` (a printf-like
 * message sink) and returns true if the display should restart. */
bool settings_backup_import(void (*report)(const char *fmt, ...));
