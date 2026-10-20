/*
 * Jellyfin client: settings, session polling, cover art and remote control.
 * See jellyfin.h. The worker task does all the network work; the LVGL side
 * only touches the data below under s_mx.
 *
 * API used (all with "Authorization: MediaBrowser ..., Token=<key>"):
 *   GET  /Sessions?ActiveWithinSeconds=960           what's playing where
 *   GET  /Items/<id>/Images/Primary?maxWidth=..      cover art (JPEG)
 *   POST /Sessions/<id>/Playing/<PlayPause|NextTrack|PreviousTrack|Stop>
 *   POST /Sessions/<id>/Playing/Seek?SeekPositionTicks=<100 ns units>
 *
 * The key is never logged. Plain http:// sends it unencrypted on the LAN.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nvs.h"

#include "jellyfin.h"
#include "json_skip.h"
#include "net_time.h"
#include "photo_frame.h"

#if __has_include("jellyfin_secrets.h")
#include "jellyfin_secrets.h"
#endif

static const char *TAG = "jellyfin";

#define NS          "jellyfin"
#define POLL_US     (2000 * 1000LL)
#define BUF_CAP     (256 * 1024)
#define TICKS_MS    10000LL          /* Jellyfin ticks are 100 ns */

typedef struct {
    enum { M_WAKE, M_CMD } type;
    jf_cmd_t cmd;
    char     session[40];
    int64_t  seek_ms;
} msg_t;

static SemaphoreHandle_t s_mx;
static QueueHandle_t     s_q;
static bool              s_loaded;
static char              s_url[JF_URL_MAX];
static char              s_key[JF_KEY_MAX];
static uint32_t          s_cfg_ver;          /* +1 when the settings change */
static bool              s_active;
static jf_snapshot_t     s_snap EXT_RAM_BSS_ATTR;   /* ~5 KB, in PSRAM */

/* Cover art: two buffers; the app shows `front`, the worker fills the other. */
static uint16_t *s_art_buf[2];
static int       s_art_front = -1, s_art_ready = -1;
static int       s_art_w[2], s_art_h[2];
static char      s_art_key[2][80];
static char      s_art_want[80];

/* ======================= settings ======================= */

static void nvs_load(void)
{
    nvs_handle_t h;
    s_url[0] = s_key[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_url);
        if (nvs_get_str(h, "url", s_url, &n) != ESP_OK) s_url[0] = '\0';
        n = sizeof(s_key);
        if (nvs_get_str(h, "key", s_key, &n) != ESP_OK) s_key[0] = '\0';
        nvs_close(h);
    }
}

static bool nvs_store(const char *url, const char *key, uint32_t seed, bool set_seed)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    if (url && url[0]) err = nvs_set_str(h, "url", url);
    else nvs_erase_key(h, "url");
    if (err == ESP_OK) {
        if (key && key[0]) err = nvs_set_str(h, "key", key);
        else nvs_erase_key(h, "key");
    }
    if (err == ESP_OK && set_seed) err = nvs_set_u32(h, "seed", seed);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "Saving the server settings failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

/* "http://host:8096/" -> "http://host:8096"; false if it isn't http(s). */
static bool clean_url(const char *in, char *out, size_t len)
{
    while (*in == ' ') in++;
    snprintf(out, len, "%s", in);
    size_t n = strlen(out);
    while (n && (out[n - 1] == '/' || out[n - 1] == ' ')) out[--n] = '\0';
    bool http = !strncmp(out, "http://", 7) && n > 7;
    bool https = !strncmp(out, "https://", 8) && n > 8;
    if (!http && !https) return false;
    for (const char *p = out; *p; p++) {
        if (*p <= ' ' || *p >= 0x7F || *p == '"' || *p == '?' || *p == '#') return false;
    }
    return true;
}

static bool key_ok(const char *key)
{
    for (const char *p = key; *p; p++) {
        if (*p <= ' ' || *p >= 0x7F || *p == '"' || *p == '\\') return false;
    }
    return true;
}

void jf_init(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (s_loaded) return;
    s_loaded = true;
#ifdef JELLYFIN_URL
    /* Copy the secrets file in when it differs from what was copied last
     * time, so editing it and reflashing takes effect, but "Forget" sticks. */
    uint32_t seed = esp_rom_crc32_le(0, (const uint8_t *)JELLYFIN_URL, strlen(JELLYFIN_URL));
    seed = esp_rom_crc32_le(seed, (const uint8_t *)JELLYFIN_API_KEY, strlen(JELLYFIN_API_KEY));
    uint32_t old = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "seed", &old);
        nvs_close(h);
    }
    char url[JF_URL_MAX];
    if (seed != old && clean_url(JELLYFIN_URL, url, sizeof(url))) {
        nvs_store(url, JELLYFIN_API_KEY, seed, true);
        ESP_LOGI(TAG, "Server settings taken from jellyfin_secrets.h");
    }
#endif
    xSemaphoreTake(s_mx, portMAX_DELAY);
    nvs_load();
    s_cfg_ver++;
    xSemaphoreGive(s_mx);
}

bool jf_configured(void)
{
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = s_url[0] && s_key[0];
    xSemaphoreGive(s_mx);
    return ok;
}

void jf_key_hint(char *out, size_t len)
{
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    size_t n = strlen(s_key);
    snprintf(out, len, "%s", n >= 8 ? s_key + n - 4 : "");
    xSemaphoreGive(s_mx);
}

void jf_get_url(char *out, size_t len)
{
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(out, len, "%s", s_url);
    xSemaphoreGive(s_mx);
}

static void wake(void)
{
    msg_t m = { .type = M_WAKE };
    if (s_q) xQueueSend(s_q, &m, 0);
}

bool jf_save(const char *url, const char *key, char *why, size_t why_len)
{
    jf_init();
    char u[JF_URL_MAX];
    if (!clean_url(url, u, sizeof(u))) {
        snprintf(why, why_len, "The address must start with http:// or https://");
        return false;
    }
    if (key && !key_ok(key)) {
        snprintf(why, why_len, "The API key has characters that can't be right");
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = nvs_store(u, key && key[0] ? key : s_key, 0, false);
    if (ok) {
        snprintf(s_url, sizeof(s_url), "%s", u);
        if (key && key[0]) snprintf(s_key, sizeof(s_key), "%s", key);
        s_cfg_ver++;
        s_snap.state = JF_CONNECTING;
        s_snap.n = 0;
        s_snap.version++;
    }
    xSemaphoreGive(s_mx);
    if (!ok) snprintf(why, why_len, "Saving failed. Try again.");
    wake();
    return ok;
}

void jf_forget(void)
{
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    nvs_store(NULL, NULL, 0, false);
    memset(s_key, 0, sizeof(s_key));
    s_url[0] = '\0';
    s_cfg_ver++;
    s_snap.state = JF_NOT_SET;
    s_snap.n = 0;
    s_snap.version++;
    xSemaphoreGive(s_mx);
}

/* ======================= HTTP ======================= */

typedef struct {
    char       *buf;
    int         len, cap;
    bool        overflow;
    json_skip_t *js;           /* non-NULL: filter the answer through it */
} http_buf_t;

/* Parts of the /Sessions answer we never use. The web player alone sends
 * its whole queue (NowPlayingQueueFullItems: 800+ KB for 100 episodes). */
static const char *const DROP[] = {
    "NowPlayingQueueFullItems", "NowPlayingQueue", "MediaStreams", "MediaSources",
    "Chapters", "People", "ImageBlurHashes", "ExternalUrls", "Overview",
    "TranscodingInfo", "Capabilities", "SupportedCommands", NULL,
};

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_buf_t *b = evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b && b->js) {
        size_t len = b->len;
        json_skip_feed(b->js, evt->data, evt->data_len, b->buf, &len, b->cap + 1);
        b->len = len;
        if (b->js->overflow) b->overflow = true;
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + evt->data_len <= b->cap) {
            memcpy(b->buf + b->len, evt->data, evt->data_len);
            b->len += evt->data_len;
            b->buf[b->len] = '\0';
        } else {
            b->overflow = true;
        }
    }
    return ESP_OK;
}

/* Sends a request to <url><path>; returns the HTTP status, -1 if it failed
 * or -2 if the answer didn't fit. The path is logged on failure, never the key. */
static int http_do(const char *path, bool post, http_buf_t *hb)
{
    char url[JF_URL_MAX + 160], auth[JF_KEY_MAX + 140];
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(url, sizeof(url), "%s%s", s_url, path);
    snprintf(auth, sizeof(auth), "MediaBrowser Client=\"ESP32 Dashboard\", "
             "Device=\"ESP32-S3 display\", DeviceId=\"esp32-s3-dashboard\", "
             "Version=\"1.0\", Token=\"%s\"", s_key);
    xSemaphoreGive(s_mx);

    hb->len = 0;
    hb->overflow = false;
    hb->buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler     = http_event,
        .user_data         = hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 8000,
        .buffer_size       = 4096,
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        memset(auth, 0, sizeof(auth));
        return -1;
    }
    esp_http_client_set_header(client, "Authorization", auth);
    memset(auth, 0, sizeof(auth));
    if (post) esp_http_client_set_post_field(client, "", 0);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%.40s: %s", path, esp_err_to_name(err));
        return -1;
    }
    if (hb->overflow) {
        ESP_LOGW(TAG, "%.40s: answer too large", path);
        return -2;
    }
    return status;
}

static void status_text(int status, char *out, size_t len)
{
    if (status == -1)       snprintf(out, len, "Can't reach the server");
    else if (status == -2)  snprintf(out, len, "The server's answer was too large");
    else if (status == 401) snprintf(out, len, "The server didn't accept the API key");
    else if (status == 403) snprintf(out, len, "The API key isn't allowed to do that");
    else if (status == 404) snprintf(out, len, "That address isn't a Jellyfin server");
    else                    snprintf(out, len, "The server answered with error %d", status);
}

/* ======================= text helpers ======================= */

/* UTF-8 -> what the fonts can show: accented Latin letters lose the accent,
 * smart quotes and dashes become plain ones, anything else becomes '?'. */
static void to_ascii(const char *in, char *out, size_t len)
{
    static const char latin1[] =                 /* U+00C0 .. U+00FF */
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
        "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 4 < len) {
        if (*p < 0x80) {
            out[o++] = (char)*p++;
        } else if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
            out[o++] = latin1[p[1] - 0x80];
            p += 2;
        } else if (p[0] == 0xE2 && p[1] == 0x80 && p[2]) {
            unsigned c = p[2];
            if (c == 0x98 || c == 0x99)      out[o++] = '\'';
            else if (c == 0x9C || c == 0x9D) out[o++] = '"';
            else if (c == 0x93 || c == 0x94) out[o++] = '-';
            else if (c == 0xA6) { memcpy(out + o, "...", 3); o += 3; }
            else out[o++] = '?';
            p += 3;
        } else {
            out[o++] = '?';
            p++;                                 /* skip the rest of the sequence */
            while ((*p & 0xC0) == 0x80) p++;
        }
    }
    out[o] = '\0';
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(it) ? it->valuestring : "";
}

static double jnum(const cJSON *o, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(it) ? it->valuedouble : 0;
}

static bool jbool(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

static void art_of(const cJSON *npi, const char *id_key, const char *tag_key,
                   char *out, size_t len)
{
    const char *id = jstr(npi, id_key), *tag = jstr(npi, tag_key);
    if (id[0] && tag[0] && !out[0]) snprintf(out, len, "%.36s:%.40s", id, tag);
}

/* One session that's playing something -> s. */
static void parse_session(const cJSON *ss, const cJSON *npi, jf_session_t *s)
{
    char tmp[160];
    memset(s, 0, sizeof(*s));
    snprintf(s->id, sizeof(s->id), "%s", jstr(ss, "Id"));
    snprintf(s->item, sizeof(s->item), "%s", jstr(npi, "Id"));

    snprintf(tmp, sizeof(tmp), "%s on %s", jstr(ss, "UserName")[0] ? jstr(ss, "UserName")
             : "Someone", jstr(ss, "DeviceName")[0] ? jstr(ss, "DeviceName") : jstr(ss, "Client"));
    to_ascii(tmp, s->who, sizeof(s->who));
    to_ascii(jstr(ss, "Client"), s->client, sizeof(s->client));
    to_ascii(jstr(npi, "Name"), s->title, sizeof(s->title));

    const char *type = jstr(npi, "Type");
    s->audio = !strcmp(type, "Audio") || !strcmp(type, "AudioBook");
    int year = (int)jnum(npi, "ProductionYear");
    if (s->audio) {
        tmp[0] = '\0';
        const cJSON *artists = cJSON_GetObjectItemCaseSensitive(npi, "Artists");
        const cJSON *a;
        cJSON_ArrayForEach(a, artists) {
            if (cJSON_IsString(a)) {
                size_t n = strlen(tmp);
                snprintf(tmp + n, sizeof(tmp) - n, "%s%s", n ? ", " : "", a->valuestring);
            }
        }
        if (!tmp[0]) snprintf(tmp, sizeof(tmp), "%s", jstr(npi, "AlbumArtist"));
        to_ascii(tmp, s->line2, sizeof(s->line2));
        if (year) snprintf(tmp, sizeof(tmp), "%s (%d)", jstr(npi, "Album"), year);
        else snprintf(tmp, sizeof(tmp), "%s", jstr(npi, "Album"));
        to_ascii(tmp, s->line3, sizeof(s->line3));
        art_of(npi, "AlbumId", "AlbumPrimaryImageTag", s->art, sizeof(s->art));
    } else if (!strcmp(type, "Episode")) {
        to_ascii(jstr(npi, "SeriesName"), s->line2, sizeof(s->line2));
        int se = (int)jnum(npi, "ParentIndexNumber"), ep = (int)jnum(npi, "IndexNumber");
        if (se && ep) snprintf(s->line3, sizeof(s->line3), "Season %d, episode %d", se, ep);
        else if (ep)  snprintf(s->line3, sizeof(s->line3), "Episode %d", ep);
        art_of(npi, "SeriesId", "SeriesPrimaryImageTag", s->art, sizeof(s->art));
    } else {
        if (year) snprintf(s->line2, sizeof(s->line2), "%d", year);
        if (strcmp(type, "Movie")) to_ascii(type, s->line3, sizeof(s->line3));
    }
    /* The item's own picture (ImageTags.Primary) as the fallback. */
    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(npi, "ImageTags");
    const char *tag = jstr(tags, "Primary");
    if (!s->art[0] && tag[0]) snprintf(s->art, sizeof(s->art), "%.36s:%.40s", s->item, tag);

    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(ss, "PlayState");
    s->runtime_ms = (int64_t)(jnum(npi, "RunTimeTicks") / TICKS_MS);
    s->pos_ms = (int64_t)(jnum(ps, "PositionTicks") / TICKS_MS);
    s->paused = jbool(ps, "IsPaused");
    s->can_seek = jbool(ps, "CanSeek");
    s->can_control = jbool(ss, "SupportsRemoteControl");
}

/* ======================= worker ======================= */

static void *psram_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void set_state(jf_state_t st, const char *msg)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_snap.state != st || strcmp(s_snap.msg, msg)) {
        s_snap.state = st;
        snprintf(s_snap.msg, sizeof(s_snap.msg), "%s", msg);
        if (st != JF_OK) s_snap.n = 0;
        s_snap.version++;
    }
    xSemaphoreGive(s_mx);
}

static void poll_sessions(http_buf_t *hb)
{
    uint32_t ver = s_cfg_ver;
    json_skip_t js;
    json_skip_init(&js, DROP);
    hb->js = &js;
    int st = http_do("/Sessions?ActiveWithinSeconds=960", false, hb);
    hb->js = NULL;
    if (st != 200) {
        char msg[96];
        status_text(st, msg, sizeof(msg));
        if (ver == s_cfg_ver) set_state(JF_ERROR, msg);
        return;
    }
    cJSON *root = cJSON_Parse(hb->buf);
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        if (ver == s_cfg_ver) set_state(JF_ERROR, "The server's answer didn't make sense");
        return;
    }
    static jf_session_t list[JF_SESSIONS] EXT_RAM_BSS_ATTR;
    int n = 0;
    const cJSON *ss;
    cJSON_ArrayForEach(ss, root) {
        const cJSON *npi = cJSON_GetObjectItemCaseSensitive(ss, "NowPlayingItem");
        if (cJSON_IsObject(npi) && n < JF_SESSIONS) parse_session(ss, npi, &list[n++]);
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (ver == s_cfg_ver) {                     /* settings didn't change meanwhile */
        memcpy(s_snap.s, list, sizeof(list[0]) * n);
        s_snap.n = n;
        s_snap.state = JF_OK;
        s_snap.msg[0] = '\0';
        s_snap.at_us = esp_timer_get_time();
        s_snap.version++;
    }
    xSemaphoreGive(s_mx);
}

static void send_command(const msg_t *m, http_buf_t *hb)
{
    static const char *const names[] = {
        [JF_CMD_PLAYPAUSE] = "PlayPause", [JF_CMD_NEXT] = "NextTrack",
        [JF_CMD_PREV] = "PreviousTrack", [JF_CMD_STOP] = "Stop", [JF_CMD_SEEK] = "Seek",
    };
    char path[160];
    if (m->cmd == JF_CMD_SEEK) {
        snprintf(path, sizeof(path), "/Sessions/%s/Playing/Seek?SeekPositionTicks=%lld",
                 m->session, (long long)(m->seek_ms * TICKS_MS));
    } else {
        snprintf(path, sizeof(path), "/Sessions/%s/Playing/%s", m->session, names[m->cmd]);
    }
    int st = http_do(path, true, hb);
    if (st < 200 || st > 299) ESP_LOGW(TAG, "%s: %d", names[m->cmd], st);
}

static void fetch_art(http_buf_t *hb)
{
    char want[80];
    int back;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(want, sizeof(want), "%s", s_art_want);
    int have = s_art_ready >= 0 ? s_art_ready : s_art_front;
    bool done = !want[0] || (have >= 0 && !strcmp(s_art_key[have], want));
    back = s_art_front == 0 ? 1 : 0;
    if (!done && s_art_ready == back) s_art_ready = -1;   /* about to overwrite it */
    xSemaphoreGive(s_mx);
    if (done) return;

    if (!s_art_buf[back]) {
        s_art_buf[back] = heap_caps_malloc(JF_ART_W * JF_ART_H * 2, MALLOC_CAP_SPIRAM);
        if (!s_art_buf[back]) return;
    }
    char id[40], path[160];
    const char *colon = strchr(want, ':');
    snprintf(id, sizeof(id), "%.*s", colon ? (int)(colon - want) : 0, want);
    snprintf(path, sizeof(path), "/Items/%s/Images/Primary?maxWidth=%d&maxHeight=%d"
             "&quality=85&format=Jpg&tag=%s", id, JF_ART_W, JF_ART_H, colon ? colon + 1 : "");
    int st = http_do(path, false, hb);
    char err[64];
    int w = 0, h = 0;
    bool ok = st == 200 && photo_decode_mem(hb->buf, hb->len, s_art_buf[back],
                                            JF_ART_W, JF_ART_H, err, &w, &h);
    if (!ok) {
        ESP_LOGW(TAG, "Cover art: %s", st == 200 ? err : "download failed");
        w = h = 0;                              /* publish "no picture" for this key */
    } else {
        /* The picture is centred in the box; move it to the start, packed. */
        uint16_t *px = s_art_buf[back];
        int x0 = (JF_ART_W - w) / 2, y0 = (JF_ART_H - h) / 2;
        for (int r = 0; r < h; r++) {
            memmove(px + r * w, px + (y0 + r) * JF_ART_W + x0, (size_t)w * 2);
        }
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(s_art_key[back], sizeof(s_art_key[back]), "%s", want);
    s_art_w[back] = w;
    s_art_h[back] = h;
    s_art_ready = back;
    xSemaphoreGive(s_mx);
}

/* While the app is closed, give back the download buffer and cover art
 * (about 0.6 MB of PSRAM); they're allocated again when it opens. */
static void release_buffers(http_buf_t *hb)
{
    free(hb->buf);
    hb->buf = NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < 2; i++) {
        free(s_art_buf[i]);
        s_art_buf[i] = NULL;
        s_art_key[i][0] = '\0';
    }
    s_art_front = s_art_ready = -1;
    s_art_want[0] = '\0';
    xSemaphoreGive(s_mx);
}

static void worker(void *arg)
{
    (void)arg;
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);
    http_buf_t hb = { .cap = BUF_CAP };
    int64_t next_poll = 0;

    for (;;) {
        bool active = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
        if (!active && hb.buf) release_buffers(&hb);
        msg_t m;
        if (xQueueReceive(s_q, &m, active ? pdMS_TO_TICKS(200) : portMAX_DELAY)) {
            if (m.type == M_WAKE) next_poll = 0;
        } else {
            m.type = M_WAKE;
        }
        if (!hb.buf) hb.buf = heap_caps_malloc(BUF_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!hb.buf) continue;
        if (!jf_configured()) {
            set_state(JF_NOT_SET, "");
            continue;
        }
        if (!net_time_wifi_connected()) {
            set_state(JF_NO_WIFI, "");
            continue;
        }
        if (m.type == M_CMD) {
            send_command(&m, &hb);
            next_poll = esp_timer_get_time() + 300 * 1000;   /* show the result soon */
            continue;
        }
        active = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
        if (active && esp_timer_get_time() >= next_poll) {
            poll_sessions(&hb);
            next_poll = esp_timer_get_time() + POLL_US;
        }
        if (active) fetch_art(&hb);
    }
}

/* ======================= public, LVGL side ======================= */

void jf_set_active(bool on)
{
    jf_init();
    if (!s_q) {
        s_q = xQueueCreate(8, sizeof(msg_t));
        xSemaphoreTake(s_mx, portMAX_DELAY);
        s_snap.state = JF_CONNECTING;
        xSemaphoreGive(s_mx);
        if (xTaskCreateWithCaps(worker, "jellyfin", 8192, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "Could not start the Jellyfin task");
        }
    }
    __atomic_store_n(&s_active, on, __ATOMIC_RELEASE);
    if (on) wake();
}

void jf_snapshot(jf_snapshot_t *out)
{
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_mx);
}

void jf_command(const char *session_id, jf_cmd_t cmd, int64_t seek_ms)
{
    if (!s_q) return;
    msg_t m = { .type = M_CMD, .cmd = cmd, .seek_ms = seek_ms };
    snprintf(m.session, sizeof(m.session), "%s", session_id);
    xQueueSend(s_q, &m, 0);
}

void jf_art_want(const char *art)
{
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(s_art_want, sizeof(s_art_want), "%s", art);
    xSemaphoreGive(s_mx);
}

bool jf_art_take(char *art, size_t art_len, const uint16_t **px, int *w, int *h)
{
    bool got = false;
    jf_init();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_art_ready >= 0) {
        s_art_front = s_art_ready;
        s_art_ready = -1;
        snprintf(art, art_len, "%s", s_art_key[s_art_front]);
        *px = s_art_buf[s_art_front];
        *w = s_art_w[s_art_front];
        *h = s_art_h[s_art_front];
        got = true;
    }
    xSemaphoreGive(s_mx);
    return got;
}
