#pragma once
/*
 * Trust for SD card apps (.app files, see tools/appkey.py for the format):
 * trusted signing keys, developer mode, and checking an app file.
 *
 * - Built-in keys: every .pub file in the project's keys/ folder, compiled into the
 *   firmware (up to 4; builtin_app_key.pub shows as "Built-in").
 * - Added keys: a .pub file in dashboard/import is read at start-up and the
 *   user is asked on screen (launcher) before it's trusted. A file on the
 *   card alone can never add trust.
 * - Developer mode (off by default): unsigned apps may run, each one after
 *   the user approves that exact file (its SHA-256). Turning it off forgets
 *   the approvals.
 *
 * Everything is kept in NVS namespace "appsec", NOT in "settings", so a
 * settings backup (restored from the card without asking) can't add keys
 * or turn developer mode on. Thread-safe.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPK_MAX_KEYS   8           /* added keys, plus the built-in one */
#define APPK_NAME_MAX   32
#define APPK_HDR        128         /* .app header size */
#define APPK_MAX_FILE   (2 * 1024 * 1024)
#define APPK_ICON_MAX   64          /* tile picture, pixels per side */

typedef struct {
    char    name[APPK_NAME_MAX];
    uint8_t id[8];                  /* first 8 bytes of SHA-256 of the key (DER) */
    bool    builtin;
} appk_key_t;

typedef enum {
    APPK_OK,                        /* signed by a trusted key */
    APPK_OK_DEV,                    /* unsigned, developer mode on, approved */
    APPK_NEEDS_APPROVAL,            /* unsigned, developer mode on, not approved yet */
    APPK_REFUSED,                   /* see why */
} appk_status_t;

typedef struct {
    char     name[APPK_NAME_MAX];
    char     version[16];
    char     symbol[8];             /* UTF-8 tile symbol */
    uint32_t color;
    bool     is_signed;
    uint8_t  key_id[8];
    char     signer[APPK_NAME_MAX]; /* trusted key's name, if signed by one */
    uint8_t  sha[32];               /* SHA-256 of header + code */
    const uint8_t *elf;             /* points into the checked data */
    size_t   elf_len;
    char     folder[16];            /* "Games", or "" (Custom Apps) */
    const uint8_t *icon;            /* LVGL TRUE_COLOR_ALPHA pixels, or NULL */
    uint16_t icon_w, icon_h;        /* at most APPK_ICON_MAX */
} appk_info_t;

void app_keys_init(void);

int  app_keys_count(void);
bool app_keys_get(int i, appk_key_t *out);
bool app_keys_remove(int i);        /* added keys only */
/* "D2D0 FEB1 EA31 5ADC" */
void app_keys_fingerprint(const uint8_t id[8], char *out, size_t len);

bool app_keys_dev_mode(void);
void app_keys_set_dev_mode(bool on);
bool app_keys_approve(const uint8_t sha[32]);

/* Checks an app file held in memory. Fills info (as far as the header could
 * be read) and, unless APPK_OK/APPK_OK_DEV, why. */
appk_status_t app_keys_check(const uint8_t *data, size_t len, appk_info_t *info,
                             char *why, size_t why_len);

/* SD task: reads dir/<name>.pub files, queues them for the user to accept
 * or reject on screen, and deletes them. */
void app_keys_import_dir(const char *dir);

/* LVGL side: the next key waiting for an answer. */
bool app_keys_pending(char *name, size_t name_len, uint8_t id[8]);
/* Answers the pending key: trusts it if yes, under name (the user's; NULL
 * or empty keeps the file's). False if it couldn't be saved. */
bool app_keys_answer(bool yes, const char *name);
