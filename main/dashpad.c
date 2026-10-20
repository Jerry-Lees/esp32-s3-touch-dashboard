/*
 * DashPad engine; see dashpad.h. The layout lives in s_deck (PSRAM) and is
 * only touched in the LVGL task; the worker task gets copies of actions and
 * reports back through a queue. The pairing secret is never logged.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "nvs.h"

#include "lvgl.h"
#include "dashpad.h"
#include "launcher.h"
#include "net_time.h"
#include "sd_card.h"
#include "settings.h"
#include "usb_kbd.h"

static const char *TAG = "dashpad";

#define NS        "dashpad"
#define JSON_MAX  (16 * 1024)
#define CHECK_MS  10000

extern const char default_json_start[] asm("_binary_dashpad_json_start");
extern const char default_json_end[]   asm("_binary_dashpad_json_end");

/* ======================= symbols ======================= */

static const struct { const char *name, *sym; } SYMBOLS[] = {
    { "play", LV_SYMBOL_PLAY }, { "pause", LV_SYMBOL_PAUSE }, { "stop", LV_SYMBOL_STOP },
    { "prev", LV_SYMBOL_PREV }, { "next", LV_SYMBOL_NEXT }, { "audio", LV_SYMBOL_AUDIO },
    { "mute", LV_SYMBOL_MUTE }, { "volume", LV_SYMBOL_VOLUME_MAX }, { "video", LV_SYMBOL_VIDEO },
    { "image", LV_SYMBOL_IMAGE }, { "keyboard", LV_SYMBOL_KEYBOARD }, { "file", LV_SYMBOL_FILE },
    { "directory", LV_SYMBOL_DIRECTORY }, { "edit", LV_SYMBOL_EDIT }, { "save", LV_SYMBOL_SAVE },
    { "home", LV_SYMBOL_HOME }, { "settings", LV_SYMBOL_SETTINGS }, { "power", LV_SYMBOL_POWER },
    { "refresh", LV_SYMBOL_REFRESH }, { "loop", LV_SYMBOL_LOOP }, { "shuffle", LV_SYMBOL_SHUFFLE },
    { "eye", LV_SYMBOL_EYE_OPEN }, { "eye_close", LV_SYMBOL_EYE_CLOSE }, { "gps", LV_SYMBOL_GPS },
    { "wifi", LV_SYMBOL_WIFI }, { "bluetooth", LV_SYMBOL_BLUETOOTH }, { "usb", LV_SYMBOL_USB },
    { "bell", LV_SYMBOL_BELL }, { "envelope", LV_SYMBOL_ENVELOPE }, { "call", LV_SYMBOL_CALL },
    { "charge", LV_SYMBOL_CHARGE }, { "battery", LV_SYMBOL_BATTERY_FULL },
    { "download", LV_SYMBOL_DOWNLOAD }, { "upload", LV_SYMBOL_UPLOAD }, { "trash", LV_SYMBOL_TRASH },
    { "copy", LV_SYMBOL_COPY }, { "cut", LV_SYMBOL_CUT }, { "paste", LV_SYMBOL_PASTE },
    { "list", LV_SYMBOL_LIST }, { "bars", LV_SYMBOL_BARS }, { "ok", LV_SYMBOL_OK },
    { "close", LV_SYMBOL_CLOSE }, { "plus", LV_SYMBOL_PLUS }, { "minus", LV_SYMBOL_MINUS },
    { "warning", LV_SYMBOL_WARNING }, { "up", LV_SYMBOL_UP }, { "down", LV_SYMBOL_DOWN },
    { "left", LV_SYMBOL_LEFT }, { "right", LV_SYMBOL_RIGHT }, { "sd", LV_SYMBOL_SD_CARD },
};
#define N_SYMBOLS ((int)(sizeof(SYMBOLS) / sizeof(SYMBOLS[0])))

int dashpad_symbol_count(void) { return N_SYMBOLS; }
const char *dashpad_symbol_name(int i) { return i >= 0 && i < N_SYMBOLS ? SYMBOLS[i].name : ""; }

const char *dashpad_symbol(const char *name)
{
    for (int i = 0; i < N_SYMBOLS; i++) {
        if (!strcmp(SYMBOLS[i].name, name)) return SYMBOLS[i].sym;
    }
    return NULL;
}

/* ======================= JSON <-> layout ======================= */

static bool printable(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) return false;
    }
    return true;
}

static bool pc_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 31) return false;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return false;
    }
    return true;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

/* Reads one action; allow_multi/page limit what's allowed (steps can't nest). */
static bool parse_action(const cJSON *j, dp_action_t *a, dp_action_t *steps, uint8_t *nsteps,
                         bool top, char *why, size_t wl)
{
    memset(a, 0, sizeof(*a));
    const char *type = j ? jstr(j, "type") : "none";
    if (!type || !strcmp(type, "none")) return true;
    if (!strcmp(type, "pc")) {
        const char *n = jstr(j, "name");
        if (!n || !pc_name_ok(n)) {
            snprintf(why, wl, "a \"pc\" action needs a \"name\" of letters, digits, - or _");
            return false;
        }
        const char *k = jstr(j, "keys");
        char e[64];
        if (k && (strlen(k) >= sizeof(a->keys) || !usb_kbd_parse(k, e, sizeof(e)))) {
            snprintf(why, wl, "keys \"%s\": %s", k, strlen(k) >= sizeof(a->keys) ? "too long" : e);
            return false;
        }
        a->kind = DP_PC;
        snprintf(a->text, sizeof(a->text), "%s", n);
        if (k) snprintf(a->keys, sizeof(a->keys), "%s", k);
    } else if (!strcmp(type, "url")) {
        const char *u = jstr(j, "url"), *m = jstr(j, "method"), *b = jstr(j, "body");
        if (!u || (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) ||
            strlen(u) >= sizeof(a->text) || !printable(u)) {
            snprintf(why, wl, "a \"url\" action needs an http:// or https:// \"url\" "
                     "(under %d characters)", (int)sizeof(a->text));
            return false;
        }
        if (b && (strlen(b) >= sizeof(a->body) || !printable(b))) {
            snprintf(why, wl, "the \"body\" is too long (max %d)", (int)sizeof(a->body) - 1);
            return false;
        }
        a->kind = DP_URL;
        a->post = m && !strcasecmp(m, "POST");
        snprintf(a->text, sizeof(a->text), "%s", u);
        if (b) snprintf(a->body, sizeof(a->body), "%s", b);
    } else if (!strcmp(type, "keys")) {
        const char *k = jstr(j, "keys");
        char e[64];
        if (!k || strlen(k) >= 64 || !usb_kbd_parse(k, e, sizeof(e))) {
            snprintf(why, wl, "keys \"%s\": %s", k ? k : "", k ? e : "missing");
            return false;
        }
        a->kind = DP_KEYS;
        snprintf(a->text, sizeof(a->text), "%s", k);
    } else if (!strcmp(type, "page") && top) {
        const char *p = jstr(j, "page");
        if (!p || !*p || strlen(p) >= DP_LABEL) {
            snprintf(why, wl, "a \"page\" action needs the \"page\" name");
            return false;
        }
        a->kind = DP_PAGE;
        snprintf(a->text, sizeof(a->text), "%s", p);
    } else if (!strcmp(type, "multi") && top) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "steps"), *s;
        int n = cJSON_GetArraySize(st);
        if (!cJSON_IsArray(st) || n < 1 || n > DP_STEPS) {
            snprintf(why, wl, "a \"multi\" action needs 1 to %d \"steps\"", DP_STEPS);
            return false;
        }
        a->kind = DP_MULTI;
        *nsteps = 0;
        cJSON_ArrayForEach(s, st) {
            if (!parse_action(s, &steps[*nsteps], NULL, NULL, false, why, wl)) return false;
            if (steps[*nsteps].kind != DP_PC && steps[*nsteps].kind != DP_URL &&
                steps[*nsteps].kind != DP_KEYS) {
                snprintf(why, wl, "multi steps can only be \"pc\", \"url\" or \"keys\"");
                return false;
            }
            (*nsteps)++;
        }
    } else {
        snprintf(why, wl, "unknown action type \"%s\"", type);
        return false;
    }
    return true;
}

static bool parse_deck(const char *text, dp_deck_t *d, char *why, size_t wl)
{
    memset(d, 0, sizeof(*d));
    cJSON *root = cJSON_Parse(text);
    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages"), *pg;
    const char *fmt = jstr(root, "format");
    bool ok = false;
    if (!root) {
        snprintf(why, wl, "it isn't valid JSON");
    } else if (!fmt || strcmp(fmt, "dashpad") ||
               cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "version")) != 1) {
        snprintf(why, wl, "it isn't a DashPad file (\"format\": \"dashpad\", \"version\": 1)");
    } else if (!cJSON_IsArray(pages) || cJSON_GetArraySize(pages) < 1 ||
               cJSON_GetArraySize(pages) > DP_PAGES) {
        snprintf(why, wl, "it needs 1 to %d \"pages\"", DP_PAGES);
    } else {
        ok = true;
        cJSON_ArrayForEach(pg, pages) {
            dp_page_t *p = &d->p[d->npages++];
            const char *name = jstr(pg, "name");
            snprintf(p->name, sizeof(p->name), "%s", name && *name ? name : "Page");
            const cJSON *btns = cJSON_GetObjectItemCaseSensitive(pg, "buttons"), *bj;
            if (!printable(p->name) || (btns && (!cJSON_IsArray(btns) ||
                                                 cJSON_GetArraySize(btns) > DP_BUTTONS))) {
                snprintf(why, wl, "page %d: a plain-text name and at most %d buttons, please",
                         d->npages, DP_BUTTONS);
                ok = false;
                break;
            }
            int i = 0;
            char e[100];
            cJSON_ArrayForEach(bj, btns) {
                dp_button_t *b = &p->b[i++];
                if (cJSON_IsNull(bj)) continue;             /* an empty slot */
                const char *label = jstr(bj, "label"), *sym = jstr(bj, "symbol");
                const char *col = jstr(bj, "color");
                b->used = true;
                snprintf(b->label, sizeof(b->label), "%s", label ? label : "");
                snprintf(b->symbol, sizeof(b->symbol), "%s", sym ? sym : "play");
                b->color = col ? (uint32_t)strtoul(col + (col[0] == '#'), NULL, 16) : 0x37474F;
                e[0] = '\0';
                if (!printable(b->label)) {
                    snprintf(e, sizeof(e), "labels must be plain ASCII text");
                } else if (!dashpad_symbol(b->symbol)) {
                    snprintf(e, sizeof(e), "unknown symbol \"%s\"", b->symbol);
                } else {
                    parse_action(cJSON_GetObjectItemCaseSensitive(bj, "action"), &b->act,
                                 b->steps, &b->nsteps, true, e, sizeof(e));
                }
                if (e[0]) {
                    snprintf(why, wl, "page %d, button %d: %s", d->npages, i, e);
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }
        /* Page actions must name a page that exists. */
        for (int pi = 0; ok && pi < d->npages; pi++) {
            for (int bi = 0; ok && bi < DP_BUTTONS; bi++) {
                const dp_button_t *b = &d->p[pi].b[bi];
                if (!b->used || b->act.kind != DP_PAGE) continue;
                bool found = false;
                for (int k = 0; k < d->npages; k++) found |= !strcmp(d->p[k].name, b->act.text);
                if (!found) {
                    snprintf(why, wl, "page %d, button %d: there's no page \"%s\"", pi + 1, bi + 1,
                             b->act.text);
                    ok = false;
                }
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}

static cJSON *action_json(const dp_action_t *a, const dp_action_t *steps, int nsteps)
{
    cJSON *j = cJSON_CreateObject();
    switch (a->kind) {
    case DP_PC:
        cJSON_AddStringToObject(j, "type", "pc");
        cJSON_AddStringToObject(j, "name", a->text);
        if (a->keys[0]) cJSON_AddStringToObject(j, "keys", a->keys);
        break;
    case DP_URL:
        cJSON_AddStringToObject(j, "type", "url");
        cJSON_AddStringToObject(j, "url", a->text);
        if (a->post) cJSON_AddStringToObject(j, "method", "POST");
        if (a->body[0]) cJSON_AddStringToObject(j, "body", a->body);
        break;
    case DP_KEYS:
        cJSON_AddStringToObject(j, "type", "keys");
        cJSON_AddStringToObject(j, "keys", a->text);
        break;
    case DP_PAGE:
        cJSON_AddStringToObject(j, "type", "page");
        cJSON_AddStringToObject(j, "page", a->text);
        break;
    case DP_MULTI: {
        cJSON_AddStringToObject(j, "type", "multi");
        cJSON *st = cJSON_AddArrayToObject(j, "steps");
        for (int i = 0; i < nsteps; i++) cJSON_AddItemToArray(st, action_json(&steps[i], NULL, 0));
        break;
    }
    default:
        cJSON_AddStringToObject(j, "type", "none");
        break;
    }
    return j;
}

/* The layout as JSON text (free() it). */
static char *deck_json(const dp_deck_t *d, bool pretty)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", "dashpad");
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *pages = cJSON_AddArrayToObject(root, "pages");
    for (int pi = 0; pi < d->npages; pi++) {
        cJSON *pg = cJSON_CreateObject();
        cJSON_AddStringToObject(pg, "name", d->p[pi].name);
        cJSON *btns = cJSON_AddArrayToObject(pg, "buttons");
        int last = -1;
        for (int bi = 0; bi < DP_BUTTONS; bi++) if (d->p[pi].b[bi].used) last = bi;
        for (int bi = 0; bi <= last; bi++) {
            const dp_button_t *b = &d->p[pi].b[bi];
            if (!b->used) {
                cJSON_AddItemToArray(btns, cJSON_CreateNull());
                continue;
            }
            cJSON *bj = cJSON_CreateObject();
            char col[10];
            snprintf(col, sizeof(col), "#%06lX", (unsigned long)(b->color & 0xFFFFFF));
            cJSON_AddStringToObject(bj, "label", b->label);
            cJSON_AddStringToObject(bj, "symbol", b->symbol);
            cJSON_AddStringToObject(bj, "color", col);
            cJSON_AddItemToObject(bj, "action", action_json(&b->act, b->steps, b->nsteps));
            cJSON_AddItemToArray(btns, bj);
        }
        cJSON_AddItemToArray(pages, pg);
    }
    char *s = pretty ? cJSON_Print(root) : cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

/* ======================= storage ======================= */

static dp_deck_t *s_deck;
static uint32_t   s_version;
static bool       s_reload;                /* __atomic: an import saved a new layout */

static char *nvs_read_deck(void)
{
    nvs_handle_t h;
    char *text = NULL;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = 0;
        if (nvs_get_blob(h, "deck", NULL, &n) == ESP_OK && n > 0 && n <= JSON_MAX) {
            text = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
            if (text && nvs_get_blob(h, "deck", text, &n) == ESP_OK) {
                text[n] = '\0';
            } else {
                free(text);
                text = NULL;
            }
        }
        nvs_close(h);
    }
    return text;
}

static bool nvs_write_deck(const char *text)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "deck", text, strlen(text));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving the layout failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void load_default(void)
{
    char why[120];
    size_t n = default_json_end - default_json_start;
    char *text = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (!text) return;
    memcpy(text, default_json_start, n);
    text[n] = '\0';
    if (!parse_deck(text, s_deck, why, sizeof(why))) {
        ESP_LOGE(TAG, "The built-in layout is broken: %s", why);
        memset(s_deck, 0, sizeof(*s_deck));
        s_deck->npages = 1;
        strcpy(s_deck->p[0].name, "Page 1");
    }
    free(text);
}

static void load(void)
{
    char why[120];
    char *text = nvs_read_deck();
    if (!text || !parse_deck(text, s_deck, why, sizeof(why))) {
        if (text) ESP_LOGW(TAG, "Saved layout unreadable (%s); using the default", why);
        load_default();
    }
    free(text);
    s_version++;
}

dp_deck_t *dashpad_deck(void) { return s_deck; }

bool dashpad_uses_keys(void)
{
    for (int p = 0; p < s_deck->npages; p++) {
        for (int i = 0; i < DP_BUTTONS; i++) {
            const dp_button_t *b = &s_deck->p[p].b[i];
            if (!b->used) continue;
            if (b->act.kind == DP_KEYS || (b->act.kind == DP_PC && b->act.keys[0])) return true;
            for (int k = 0; b->act.kind == DP_MULTI && k < b->nsteps; k++) {
                if (b->steps[k].kind == DP_KEYS) return true;
            }
        }
    }
    return false;
}
uint32_t dashpad_version(void) { return s_version; }

bool dashpad_save(void)
{
    char *text = deck_json(s_deck, false);
    bool ok = text && strlen(text) <= JSON_MAX && nvs_write_deck(text);
    free(text);
    return ok;
}

void dashpad_reset_default(void)
{
    load_default();
    dashpad_save();
    s_version++;
}

bool dashpad_export(char *path, size_t len)
{
    if (!sd_card_mounted()) return false;
    char *text = deck_json(s_deck, true);
    if (!text) return false;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    snprintf(path, len, SD_DIR "/backup/dashpad_%s.json", stamp);
    FILE *f = fopen(path, "w");
    bool ok = f && fputs(text, f) >= 0;
    if (f) ok &= fclose(f) == 0;
    free(text);
    return ok;
}

void dashpad_import_dir(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        size_t n = strlen(de->d_name);
        if (strncasecmp(de->d_name, "dashpad", 7) || n < 12 ||
            strcasecmp(de->d_name + n - 5, ".json")) {
            continue;
        }
        char path[300], why[120];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > JSON_MAX) {
            sd_card_notice("%s not imported: it's empty or over 16 KB", de->d_name);
            continue;
        }
        char *text = heap_caps_calloc(1, st.st_size + 1, MALLOC_CAP_SPIRAM);
        dp_deck_t *d = heap_caps_malloc(sizeof(dp_deck_t), MALLOC_CAP_SPIRAM);
        FILE *f = text && d ? fopen(path, "rb") : NULL;
        bool ok = f && fread(text, 1, st.st_size, f) == (size_t)st.st_size;
        if (f) fclose(f);
        if (!ok) {
            sd_card_notice("%s not imported: couldn't read it", de->d_name);
        } else if (!parse_deck(text, d, why, sizeof(why))) {
            sd_card_notice("%s not imported: %s", de->d_name, why);
        } else {
            char *compact = deck_json(d, false);
            if (compact && nvs_write_deck(compact)) {
                unlink(path);
                __atomic_store_n(&s_reload, true, __ATOMIC_RELEASE);
                sd_card_notice("DashPad layout imported from %s", de->d_name);
            }
            free(compact);
        }
        free(text);
        free(d);
    }
    closedir(dp);
}

/* ======================= pairing ======================= */

static SemaphoreHandle_t s_mx;
static char  s_host[64];
static int   s_port;
static char  s_token[80];
static struct { char host[64]; int port; char token[80]; } s_pend;
static bool  s_pend_set;                   /* __atomic */

static void load_pairing(void)
{
    nvs_handle_t h;
    s_host[0] = s_token[0] = '\0';
    s_port = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_host);
        if (nvs_get_str(h, "host", s_host, &n) != ESP_OK) s_host[0] = '\0';
        n = sizeof(s_token);
        if (nvs_get_str(h, "token", s_token, &n) != ESP_OK) s_token[0] = '\0';
        int32_t p = 0;
        nvs_get_i32(h, "port", &p);
        s_port = p;
        nvs_close(h);
    }
}

void dashpad_pair_request(const char *host, int port, const char *token)
{
    if (__atomic_load_n(&s_pend_set, __ATOMIC_ACQUIRE)) return;
    snprintf(s_pend.host, sizeof(s_pend.host), "%s", host);
    snprintf(s_pend.token, sizeof(s_pend.token), "%s", token);
    s_pend.port = port;
    __atomic_store_n(&s_pend_set, true, __ATOMIC_RELEASE);
}

static void save_pairing(void)                 /* LVGL task: writes flash */
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    bool clear = !s_pend.host[0];
    if (clear) {
        nvs_erase_key(h, "host");
        nvs_erase_key(h, "token");
        nvs_erase_key(h, "port");
    } else {
        nvs_set_str(h, "host", s_pend.host);
        nvs_set_str(h, "token", s_pend.token);
        nvs_set_i32(h, "port", s_pend.port);
    }
    nvs_commit(h);
    nvs_close(h);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(s_host, sizeof(s_host), "%s", s_pend.host);
    snprintf(s_token, sizeof(s_token), "%s", s_pend.token);
    s_port = s_pend.port;
    xSemaphoreGive(s_mx);
    memset(&s_pend, 0, sizeof(s_pend));
    launcher_toast(clear ? "DashPad: unpaired from the PC" : "DashPad: paired with your PC",
                   4000);
}

bool dashpad_paired(char *host, size_t len, int *port)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = s_host[0] && s_token[0] && s_port > 0;
    if (host) snprintf(host, len, "%s", s_host);
    if (port) *port = s_port;
    xSemaphoreGive(s_mx);
    return ok;
}

void dashpad_unpair(void)
{
    memset(&s_pend, 0, sizeof(s_pend));
    save_pairing();
}

/* ======================= worker ======================= */

typedef struct {
    enum { J_RUN, J_CHECK } type;
    int         id;
    dp_button_t b;
} job_t;

typedef struct {
    int  id;
    bool ok;
    char msg[96];
} res_t;

static QueueHandle_t s_jobs, s_results;
static job_t         s_job EXT_RAM_BSS_ATTR;      /* LVGL side's job being queued */
static bool          s_active;                     /* __atomic */
static dp_pc_state_t s_pc = DP_PC_UNKNOWN;          /* __atomic */
static char          s_names[16][32];               /* under s_mx */
static int           s_nnames;

typedef struct {
    char *buf;
    int   len, cap;
} hbuf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    hbuf_t *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && b->len + evt->data_len < b->cap) {
        memcpy(b->buf + b->len, evt->data, evt->data_len);
        b->len += evt->data_len;
        b->buf[b->len] = '\0';
    }
    return ESP_OK;
}

/* Returns the HTTP status, or -1 if the request failed. */
static int http(const char *url, bool post, const char *body, const char *sig, hbuf_t *out)
{
    out->len = 0;
    out->buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler = http_event, .user_data = out, .timeout_ms = 6000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (post && body) {
        esp_http_client_set_header(c, "Content-Type",
                                   body[0] == '{' ? "application/json" : "text/plain");
        esp_http_client_set_post_field(c, body, strlen(body));
    }
    if (sig) esp_http_client_set_header(c, "X-DashPad-Sig", sig);
    esp_err_t err = esp_http_client_perform(c);
    int st = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return err == ESP_OK ? st : -1;
}

/* A signed request to the PC helper. *status gets the HTTP status (or -1). */
static cJSON *pc_request(const char *path, cJSON *req, int *status, hbuf_t *out)
{
    char host[64], token[80], url[128], nonce[33], sig[65];
    int port;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(host, sizeof(host), "%s", s_host);
    snprintf(token, sizeof(token), "%s", s_token);
    port = s_port;
    xSemaphoreGive(s_mx);
    uint8_t rnd[16], mac[32];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) sprintf(nonce + 2 * i, "%02x", rnd[i]);
    cJSON_AddNumberToObject(req, "ts", (double)time(NULL));
    cJSON_AddStringToObject(req, "nonce", nonce);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        *status = -1;
        return NULL;
    }
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t *)token,
                    strlen(token), (const uint8_t *)body, strlen(body), mac);
    memset(token, 0, sizeof(token));
    for (int i = 0; i < 32; i++) sprintf(sig + 2 * i, "%02x", mac[i]);
    snprintf(url, sizeof(url), "http://%s:%d%s", host, port, path);
    *status = http(url, true, body, sig, out);
    free(body);
    return *status == 200 ? cJSON_Parse(out->buf) : NULL;
}

static bool s_helper_on = true;                    /* __atomic, from settings "dp_pc" */

bool dashpad_helper_on(void) { return __atomic_load_n(&s_helper_on, __ATOMIC_ACQUIRE); }
bool dashpad_keyboard_on(void) { return settings_get_u8("dp_kbd", 0); }
void dashpad_set_keyboard_on(bool on) { settings_set_u8("dp_kbd", on); }

void dashpad_set_helper_on(bool on)
{
    settings_set_u8("dp_pc", on);
    __atomic_store_n(&s_helper_on, on, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pc, on ? DP_PC_UNKNOWN : DP_PC_OFF, __ATOMIC_RELEASE);
    dashpad_set_active(__atomic_load_n(&s_active, __ATOMIC_ACQUIRE));   /* check now */
}

static bool can_sign(char *msg, size_t len)
{
    if (!dashpad_helper_on()) {
        snprintf(msg, len, "The PC helper is turned off (DashPad menu)");
        return false;
    }
    if (!dashpad_paired(NULL, 0, NULL)) {
        snprintf(msg, len, "Not paired with a PC (see DashPad's menu)");
        return false;
    }
    if (!net_time_is_synced()) {
        snprintf(msg, len, "The clock isn't set yet, so requests can't be signed");
        return false;
    }
    return true;
}

static bool run_pc(const char *name, char *msg, size_t len, hbuf_t *hb)
{
    if (!can_sign(msg, len)) return false;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "action", name);
    int st;
    cJSON *r = pc_request("/run", req, &st, hb);
    bool ok = r && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "ok"));
    if (!ok) {
        const char *e = r ? jstr(r, "error") : NULL;
        if (st == -1) snprintf(msg, len, "Can't reach the PC helper");
        else if (st == 403) snprintf(msg, len, "The PC refused: pair the display again");
        else snprintf(msg, len, "PC: %s", e && *e ? e : "it didn't work");
    }
    cJSON_Delete(r);
    return ok;
}

static bool run_url(const dp_action_t *a, char *msg, size_t len, hbuf_t *hb)
{
    int st = http(a->text, a->post, a->post ? a->body : NULL, NULL, hb);
    if (st >= 200 && st < 300) return true;
    if (st == -1) snprintf(msg, len, "Can't reach %.60s", a->text);
    else snprintf(msg, len, "The web request answered %d", st);
    return false;
}

/* "Run on PC": through the helper when it's switched on; otherwise (or if
 * the helper can't do it) the button's keys, if the USB keyboard is on. */
static bool run_pc_or_keys(const dp_action_t *a, char *msg, size_t len, hbuf_t *hb)
{
    bool keys = a->keys[0] && usb_kbd_active();
    if (dashpad_helper_on()) {
        if (!net_time_wifi_connected()) snprintf(msg, len, "No Wi-Fi");
        else if (run_pc(a->text, msg, len, hb)) return true;
        if (!keys) return false;
        ESP_LOGI(TAG, "Helper failed (%s); sending the keys instead", msg);
    } else if (!keys) {
        snprintf(msg, len, a->keys[0] ? "Turn on the PC helper or the USB keyboard (DashPad menu)"
                                      : "The PC helper is turned off (DashPad menu)");
        return false;
    }
    return usb_kbd_send(a->keys, msg, len);
}

static bool run_one(const dp_action_t *a, char *msg, size_t len, hbuf_t *hb)
{
    if (a->kind == DP_KEYS) return usb_kbd_send(a->text, msg, len);
    if (a->kind == DP_PC) return run_pc_or_keys(a, msg, len, hb);
    if (!net_time_wifi_connected()) {
        snprintf(msg, len, "No Wi-Fi");
        return false;
    }
    return a->kind == DP_URL ? run_url(a, msg, len, hb) : true;
}

static void check_pc(hbuf_t *hb)
{
    dp_pc_state_t state;
    if (!dashpad_helper_on()) {
        state = DP_PC_OFF;
    } else if (!dashpad_paired(NULL, 0, NULL)) {
        state = DP_PC_UNPAIRED;
    } else if (!net_time_is_synced()) {
        state = DP_PC_NOCLOCK;
    } else if (!net_time_wifi_connected()) {
        state = DP_PC_DOWN;
    } else {
        int st;
        cJSON *r = pc_request("/actions", cJSON_CreateObject(), &st, hb);
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(r, "actions"), *it;
        state = st == 200 ? DP_PC_OK : st == 403 ? DP_PC_BADKEY : DP_PC_DOWN;
        if (cJSON_IsArray(list)) {
            xSemaphoreTake(s_mx, portMAX_DELAY);
            s_nnames = 0;
            cJSON_ArrayForEach(it, list) {
                if (cJSON_IsString(it) && s_nnames < 16 && pc_name_ok(it->valuestring)) {
                    snprintf(s_names[s_nnames++], 32, "%s", it->valuestring);
                }
            }
            xSemaphoreGive(s_mx);
        }
        cJSON_Delete(r);
    }
    __atomic_store_n(&s_pc, state, __ATOMIC_RELEASE);
}

static void worker(void *arg)
{
    (void)arg;
    hbuf_t hb = { .cap = 2048 };
    hb.buf = heap_caps_malloc(hb.cap, MALLOC_CAP_SPIRAM);
    job_t *j = heap_caps_malloc(sizeof(job_t), MALLOC_CAP_SPIRAM);
    if (!hb.buf || !j) vTaskDeleteWithCaps(NULL);
    for (;;) {
        bool active = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
        if (!xQueueReceive(s_jobs, j, active ? pdMS_TO_TICKS(CHECK_MS) : portMAX_DELAY)) {
            j->type = J_CHECK;                         /* periodic check while open */
        }
        if (j->type == J_CHECK) {
            check_pc(&hb);
            continue;
        }
        res_t r = { .id = j->id, .ok = true };
        const dp_button_t *b = &j->b;
        if (b->act.kind == DP_MULTI) {
            for (int i = 0; i < b->nsteps && r.ok; i++) {
                if (i) vTaskDelay(pdMS_TO_TICKS(300));
                r.ok = run_one(&b->steps[i], r.msg, sizeof(r.msg), &hb);
            }
        } else {
            r.ok = run_one(&b->act, r.msg, sizeof(r.msg), &hb);
        }
        ESP_LOGI(TAG, "\"%s\": %s", b->label, r.ok ? "ok" : r.msg);
        xQueueSend(s_results, &r, portMAX_DELAY);
    }
}

void dashpad_run(const dp_button_t *b, int id)
{
    s_job.type = J_RUN;
    s_job.id = id;
    s_job.b = *b;
    if (xQueueSend(s_jobs, &s_job, 0) != pdTRUE) {
        res_t r = { .id = id, .ok = false };
        snprintf(r.msg, sizeof(r.msg), "Busy: try again in a moment");
        xQueueSend(s_results, &r, 0);
    }
}

bool dashpad_take_result(int *id, bool *ok, char *msg, size_t len)
{
    res_t r;
    if (!s_results || xQueueReceive(s_results, &r, 0) != pdTRUE) return false;
    *id = r.id;
    *ok = r.ok;
    snprintf(msg, len, "%s", r.msg);
    return true;
}

void dashpad_set_active(bool active)
{
    __atomic_store_n(&s_active, active, __ATOMIC_RELEASE);
    if (active && s_jobs) {
        s_job.type = J_CHECK;
        xQueueSend(s_jobs, &s_job, 0);                 /* check now */
    }
}

dp_pc_state_t dashpad_pc_state(void) { return __atomic_load_n(&s_pc, __ATOMIC_ACQUIRE); }

int dashpad_pc_actions(char names[][32], int max)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int n = s_nnames < max ? s_nnames : max;
    for (int i = 0; i < n; i++) memcpy(names[i], s_names[i], 32);
    xSemaphoreGive(s_mx);
    return n;
}

/* ======================= init ======================= */

static void service(lv_timer_t *t)                    /* LVGL task, every 500 ms */
{
    (void)t;
    if (__atomic_exchange_n(&s_reload, false, __ATOMIC_ACQ_REL)) load();
    if (__atomic_load_n(&s_pend_set, __ATOMIC_ACQUIRE)) {
        save_pairing();
        __atomic_store_n(&s_pend_set, false, __ATOMIC_RELEASE);
    }
}

void dashpad_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_deck = heap_caps_calloc(1, sizeof(dp_deck_t), MALLOC_CAP_SPIRAM);
    s_jobs = xQueueCreateWithCaps(4, sizeof(job_t), MALLOC_CAP_SPIRAM);   /* ~6 KB */
    s_results = xQueueCreate(8, sizeof(res_t));
    if (!s_deck || !s_jobs || !s_results) {
        ESP_LOGE(TAG, "Out of memory");
        return;
    }
    s_helper_on = settings_get_u8("dp_pc", 1);
    if (!s_helper_on) s_pc = DP_PC_OFF;
    load_pairing();
    load();
    lv_timer_create(service, 500, NULL);
    /* The worker never writes to flash, so its stack can be in PSRAM. */
    xTaskCreateWithCaps(worker, "dashpad", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
}
