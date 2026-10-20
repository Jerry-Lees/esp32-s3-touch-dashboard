#pragma once
/*
 * Apps loaded from the SD card: dashboard/apps/<name>.app files (an ELF file
 * with a signed header; tools/appkey.py, apps/README.md).
 *
 * The card's apps are checked (app_keys.c) at start-up; trusted ones (and,
 * in developer mode, unsigned ones) get a home screen tile. Opening a tile
 * reads and checks the file again, then loads exactly the checked bytes
 * into PSRAM with Espressif's elf_loader and runs it; closing it unloads
 * the code. One app is loaded at a time.
 */
#include <stdbool.h>
#include <stdint.h>

#include "app_framework.h"
#include "app_keys.h"

#define EXT_MAX_APPS 16

typedef struct {
    char          file[64];          /* name in dashboard/apps */
    char          name[APPK_NAME_MAX];
    char          version[16];
    char          signer[APPK_NAME_MAX]; /* trusted key's name, "" if unsigned/unknown */
    bool          is_signed;
    appk_status_t status;
    char          why[80];           /* why it's refused */
} ext_app_info_t;

/* Makes the firmware's functions available to apps. LVGL task, once. */
void ext_apps_init(void);

/* Looks at the card's apps again, in a background task (after mounting,
 * and after keys or developer mode change). */
void ext_apps_rescan(void);

/* LVGL task: applies a finished scan; true if the list changed. */
bool ext_apps_poll(void);

/* LVGL task: home screen tiles (not the refused apps). */
int ext_apps_tiles(const app_t **out, int max);

/* LVGL task: every app found, for Settings > Apps. */
int  ext_apps_count(void);
uint32_t ext_apps_version(void);   /* changes when the list does */
bool ext_apps_get(int i, ext_app_info_t *out);
