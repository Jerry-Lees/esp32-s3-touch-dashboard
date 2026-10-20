#pragma once
/*
 * Jellyfin client for the Jellyfin app (app_jellyfin.c): the server
 * settings, plus a background task that asks the server what's playing,
 * downloads cover art and sends remote-control commands. No LVGL in here;
 * the app copies jf_snapshot() from an lv_timer.
 *
 * The server address and API key are kept in their own NVS namespace
 * ("jellyfin"), not in "settings", so settings backups never contain the
 * key. The optional, gitignored main/jellyfin_secrets.h (JELLYFIN_URL,
 * JELLYFIN_API_KEY) is copied in at start-up whenever its values change,
 * i.e. after editing it and flashing again.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JF_URL_MAX  128
#define JF_KEY_MAX  80
#define JF_SESSIONS 8
#define JF_ART_W    300          /* cover art is fitted into this box */
#define JF_ART_H    300

typedef enum {
    JF_NOT_SET,                  /* no server or key saved */
    JF_NO_WIFI,
    JF_CONNECTING,               /* no answer yet */
    JF_OK,
    JF_ERROR,                    /* see msg */
} jf_state_t;

typedef struct {
    char    id[40];              /* session id */
    char    who[96];             /* "dad on Living Room TV" */
    char    client[40];          /* "Jellyfin Web", "Jellyfin Roku" ... */
    char    title[96];
    char    line2[96], line3[96];
    char    item[40];            /* item id: changes with the track/episode */
    char    art[80];             /* "itemid:tag" of the picture to show, "" = none */
    bool    audio;
    bool    paused, can_control, can_seek;
    int64_t runtime_ms, pos_ms;
} jf_session_t;

typedef struct {
    jf_state_t   state;
    char         msg[96];        /* why, for JF_ERROR */
    int          n;              /* sessions that are playing something */
    jf_session_t s[JF_SESSIONS];
    int64_t      at_us;          /* esp_timer time the positions were read */
    uint32_t     version;        /* +1 on every answer */
} jf_snapshot_t;

typedef enum {
    JF_CMD_PLAYPAUSE,
    JF_CMD_NEXT,
    JF_CMD_PREV,
    JF_CMD_STOP,
    JF_CMD_SEEK,
} jf_cmd_t;

/* Loads the settings (and the secrets file, if changed). Safe to call often. */
void jf_init(void);

bool jf_configured(void);
void jf_get_url(char *out, size_t len);
/* Last 4 characters of the saved key ("" if none), so the user can tell
 * which key is saved without showing it. */
void jf_key_hint(char *out, size_t len);
/* url must start with http:// or https://. key NULL or "" keeps the saved
 * key. False (with why) if the address isn't valid. */
bool jf_save(const char *url, const char *key, char *why, size_t why_len);
void jf_forget(void);

/* Ask the server every 2 s while on (the app is open). */
void jf_set_active(bool on);
void jf_snapshot(jf_snapshot_t *out);
void jf_command(const char *session_id, jf_cmd_t cmd, int64_t seek_ms);

/* Cover art: say which picture is wanted ("" = none); when it's ready,
 * jf_art_take() returns it once. The pixels (RGB565, w x h) stay valid
 * until the next successful jf_art_take(). */
void jf_art_want(const char *art);
bool jf_art_take(char *art, size_t art_len, const uint16_t **px, int *w, int *h);
