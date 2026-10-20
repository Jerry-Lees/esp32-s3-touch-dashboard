#pragma once
/*
 * DashPad engine: the pad layout (pages of buttons), its storage, and the
 * actions behind the buttons. The UI is app_dashpad.c.
 *
 * Layout: JSON (format "dashpad", version 1; dashpad/dashpad.json in the
 * project is the example and the built-in default). Saved in NVS namespace
 * "dashpad" (key "deck"), so it survives restarts; export it to the SD card
 * to keep a copy. A dashpad*.json in dashboard/import replaces it at the next
 * start (the file is then deleted).
 *
 * Actions: "pc" (ask the PC helper, tools/dashpad_helper.py, to run a named
 * action; requests are HMAC-SHA256 signed with the paired secret, with a
 * timestamp and nonce), "url" (GET/POST a URL), "page" (show a page) and
 * "keys" (a key combo sent as a USB keyboard, usb_kbd.c) and "multi" (up to
 * DP_STEPS pc/url/keys steps). The PC's address and secret come
 * from `dashpad_helper.py pair` over USB and are kept in the same NVS
 * namespace: not in settings backups, never logged or exported.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DP_PAGES    8
#define DP_BUTTONS  15          /* 5 x 3 */
#define DP_COLS     5
#define DP_STEPS    4
#define DP_LABEL    24
#define DP_TEXT     160

typedef enum { DP_NONE, DP_PC, DP_URL, DP_PAGE, DP_MULTI, DP_KEYS } dp_kind_t;

typedef struct {
    uint8_t kind;               /* dp_kind_t */
    bool    post;               /* url: POST instead of GET */
    char    text[DP_TEXT];      /* pc: action name; url: the URL; page: page name;
                                   keys: a combo like "ctrl+alt+t" (usb_kbd.c) */
    char    body[128];          /* url POST body */
    char    keys[64];           /* pc: keys to send instead when the helper isn't used */
} dp_action_t;

typedef struct {
    bool        used;
    char        label[DP_LABEL];
    char        symbol[16];     /* name, e.g. "keyboard" (see dashpad_symbol) */
    uint32_t    color;
    dp_action_t act;
    dp_action_t steps[DP_STEPS];
    uint8_t     nsteps;         /* multi */
} dp_button_t;

typedef struct {
    char        name[DP_LABEL];
    dp_button_t b[DP_BUTTONS];
} dp_page_t;

typedef struct {
    int       npages;
    dp_page_t p[DP_PAGES];
} dp_deck_t;

/* Loads the saved pad (or the default). LVGL task, from launcher_start(). */
void dashpad_init(void);

/* The pad (PSRAM). LVGL task only. Call dashpad_save() after changing it. */
dp_deck_t *dashpad_deck(void);
bool dashpad_save(void);
void dashpad_reset_default(void);
/* Writes the pad to SD_DIR/backup/dashpad_<date>.json; path gets the name. */
bool dashpad_export(char *path, size_t len);
/* +1 whenever the pad is replaced (import, reset): the UI rebuilds. */
uint32_t dashpad_version(void);

/* Symbol names the editor offers, and the LVGL symbol for a name. */
int dashpad_symbol_count(void);
const char *dashpad_symbol_name(int i);
const char *dashpad_symbol(const char *name);

/* SD start-up task: applies import/dashpad*.json (then deletes it). */
void dashpad_import_dir(const char *dir);

/* Pairing with the PC helper. dashpad_pair_request() may be called from any
 * task (USB); it's saved from the LVGL task. */
void dashpad_pair_request(const char *host, int port, const char *token);
bool dashpad_paired(char *host, size_t len, int *port);
void dashpad_unpair(void);

/* True if any button sends keys (DashPad then turns the USB keyboard on). */
bool dashpad_uses_keys(void);

/* Runs an action in the background (copied). id comes back with the result. */
void dashpad_run(const dp_button_t *b, int id);
/* LVGL side: the next finished action, if any. */
bool dashpad_take_result(int *id, bool *ok, char *msg, size_t len);

/* PC helper status, refreshed in the background while `active`. */
typedef enum { DP_PC_UNPAIRED, DP_PC_UNKNOWN, DP_PC_OK, DP_PC_DOWN, DP_PC_BADKEY,
               DP_PC_NOCLOCK, DP_PC_OFF } dp_pc_state_t;

/* The two ways buttons reach the PC, switched in DashPad's menu (settings
 * "dp_pc", default on, and "dp_kbd", default off: the keyboard takes over
 * the USB port while DashPad is open). */
bool dashpad_helper_on(void);
void dashpad_set_helper_on(bool on);
bool dashpad_keyboard_on(void);
void dashpad_set_keyboard_on(bool on);
void dashpad_set_active(bool active);
dp_pc_state_t dashpad_pc_state(void);
/* The helper's action names (from the last successful check). */
int dashpad_pc_actions(char names[][32], int max);
