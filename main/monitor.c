/*
 * Server monitor engine; see monitor.h. The coordinator task re-reads the
 * list when the file changes, then every `interval` seconds hands the hosts
 * to MON_WORKERS checker tasks (one host at a time each) and waits for the
 * round to finish. Results go into s_cfg under s_mx.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "app_framework.h"
#include "monitor.h"
#include "net_time.h"
#include "sd_card.h"
#include "settings.h"

static const char *TAG = "monitor";

#define FILE_PATH    SD_DIR "/monitors.json"
#define FILE_TMP     FILE_PATH ".tmp"
#define FILE_BAK     FILE_PATH ".bak"
#define FILE_MAX     (64 * 1024)
#define MON_WORKERS  3
#define PING_MS      1500
#define TCP_MS       2500
#define HTTP_MS      5000
#define ALERTS       8

static SemaphoreHandle_t s_mx;
static mon_cfg_t   *s_cfg;                 /* PSRAM */
static uint32_t     s_version;             /* __atomic */
static uint32_t     s_last_round;
static char         s_problem[96] = "Starting...";
static TaskHandle_t s_coord, s_work[MON_WORKERS];
static SemaphoreHandle_t s_done;
static int          s_next;                /* __atomic: next host to check */
static bool         s_reload;              /* __atomic */
static bool         s_save_pending;        /* __atomic */
static char         s_alert[ALERTS][96];
static int          s_alert_r, s_alert_w;
static bool         s_opt_tk = true, s_opt_pop = true, s_opt_sort = true;

void monitor_lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
void monitor_unlock(void) { xSemaphoreGive(s_mx); }
const mon_cfg_t *monitor_cfg(void) { return s_cfg; }
uint32_t monitor_version(void) { return __atomic_load_n(&s_version, __ATOMIC_ACQUIRE); }
uint32_t monitor_last_round(void) { return s_last_round; }
uint32_t monitor_now(void) { return (uint32_t)(esp_timer_get_time() / 1000000) + 1; }
static void bump(void) { __atomic_add_fetch(&s_version, 1, __ATOMIC_ACQ_REL); }

const char *monitor_problem(void) { return s_problem; }

static void set_problem(const char *p)
{
    monitor_lock();
    snprintf(s_problem, sizeof(s_problem), "%s", p);
    monitor_unlock();
    bump();
}

mon_state_t monitor_host_state(const mon_host_t *h, bool *partial)
{
    int up = 0, down = 0;
    for (int i = 0; i < h->ncheck; i++) {
        if (h->c[i].state == MON_UP) up++;
        else if (h->c[i].state == MON_DOWN) down++;
    }
    if (partial) *partial = up && down;
    if (down && !up) return MON_DOWN;
    if (up && !down) return MON_UP;
    if (up && down) return MON_UP;
    return MON_UNKNOWN;
}

/* ======================= options ======================= */

bool monitor_opt(const char *key)
{
    if (!strcmp(key, "mon_tk")) return s_opt_tk;
    if (!strcmp(key, "mon_pop")) return s_opt_pop;
    return s_opt_sort;
}

void monitor_set_opt(const char *key, bool on)       /* LVGL task: writes flash */
{
    settings_set_u8(key, on);
    if (!strcmp(key, "mon_tk")) s_opt_tk = on;
    else if (!strcmp(key, "mon_pop")) s_opt_pop = on;
    else s_opt_sort = on;
    bump();
}

/* ======================= the list file ======================= */

static bool parse_check(const char *s, mon_check_t *c, char *why, size_t wl)
{
    memset(c, 0, sizeof(*c));
    c->ms = -1;
    char kind[8] = "", arg[160] = "", label[32] = "";
    int n = sscanf(s, "%7s %159s %31[^\n]", kind, arg, label);
    if (n >= 1 && !strcasecmp(kind, "ping")) {
        c->kind = MON_PING;
        snprintf(c->label, sizeof(c->label), "Ping");
        return true;
    }
    if (n >= 2 && !strcasecmp(kind, "tcp")) {
        int port = atoi(arg);
        if (port < 1 || port > 65535) {
            snprintf(why, wl, "bad port in \"%s\"", s);
            return false;
        }
        c->kind = MON_TCP;
        c->port = port;
        if (n == 3) snprintf(c->label, sizeof(c->label), "%.19s", label);
        else snprintf(c->label, sizeof(c->label), "Port %d", port);
        return true;
    }
    if (n >= 2 && !strcasecmp(kind, "http")) {
        if ((strncmp(arg, "http://", 7) && strncmp(arg, "https://", 8)) ||
            strlen(arg) >= sizeof(c->url)) {
            snprintf(why, wl, "\"http\" needs an http:// or https:// URL");
            return false;
        }
        c->kind = MON_HTTP;
        snprintf(c->url, sizeof(c->url), "%.127s", arg);
        snprintf(c->label, sizeof(c->label), "%.19s", n == 3 ? label : "Web");
        return true;
    }
    snprintf(why, wl, "unknown check \"%.40s\" (ping, tcp, http)", s);
    return false;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static bool parse_list(const char *text, mon_cfg_t *cfg, char *why, size_t wl)
{
    memset(cfg, 0, sizeof(*cfg));
    cJSON *root = cJSON_Parse(text);
    const char *fmt = jstr(root, "format");
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(root, "groups"), *g;
    bool ok = true;
    if (!root) {
        snprintf(why, wl, "monitors.json isn't valid JSON");
        ok = false;
    } else if (!fmt || strcmp(fmt, "dashboard-monitor") ||
               cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "version")) != 1) {
        snprintf(why, wl, "monitors.json needs \"format\": \"dashboard-monitor\", \"version\": 1");
        ok = false;
    } else if (!cJSON_IsArray(groups)) {
        snprintf(why, wl, "monitors.json has no \"groups\" list");
        ok = false;
    } else {
        double iv = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, "interval"));
        cfg->interval = iv >= 15 && iv <= 3600 ? (int)iv : 60;
        cJSON_ArrayForEach(g, groups) {
            if (cfg->ngroups == MON_GROUPS) {
                snprintf(why, wl, "more than %d groups", MON_GROUPS);
                ok = false;
                break;
            }
            int gi = cfg->ngroups++;
            const char *gn = jstr(g, "name");
            snprintf(cfg->group[gi], sizeof(cfg->group[gi]), "%s", gn && *gn ? gn : "Servers");
            const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(g, "hosts"), *h;
            cJSON_ArrayForEach(h, hosts) {
                if (cfg->nhosts == MON_HOSTS) {
                    snprintf(why, wl, "more than %d hosts", MON_HOSTS);
                    ok = false;
                    break;
                }
                mon_host_t *m = &cfg->h[cfg->nhosts];
                const char *name = jstr(h, "name"), *addr = jstr(h, "host"), *what = jstr(h, "what");
                const cJSON *checks = cJSON_GetObjectItemCaseSensitive(h, "checks"), *c;
                if (!name || !*name || !addr || !*addr) {
                    snprintf(why, wl, "group \"%s\": every host needs \"name\" and \"host\"",
                             cfg->group[gi]);
                    ok = false;
                    break;
                }
                m->group = gi;
                snprintf(m->name, sizeof(m->name), "%s", name);
                snprintf(m->addr, sizeof(m->addr), "%s", addr);
                snprintf(m->what, sizeof(m->what), "%s", what ? what : "");
                if (!cJSON_IsArray(checks) || cJSON_GetArraySize(checks) == 0) {
                    parse_check("ping", &m->c[m->ncheck++], why, wl);   /* default */
                }
                cJSON_ArrayForEach(c, checks) {
                    char e[80];
                    if (m->ncheck == MON_CHECKS) break;
                    if (!cJSON_IsString(c) || !parse_check(c->valuestring, &m->c[m->ncheck], e, sizeof(e))) {
                        snprintf(why, wl, "%s: %s", name, cJSON_IsString(c) ? e : "checks are strings");
                        ok = false;
                        break;
                    }
                    m->ncheck++;
                }
                if (!ok) break;
                cfg->nhosts++;
            }
            if (!ok) break;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* Keeps the results of hosts/checks that are still in the new list. */
static void carry_over(mon_cfg_t *nw, const mon_cfg_t *old)
{
    for (int i = 0; i < nw->nhosts; i++) {
        for (int j = 0; j < old->nhosts; j++) {
            if (strcmp(nw->h[i].name, old->h[j].name) || strcmp(nw->h[i].addr, old->h[j].addr)) continue;
            for (int a = 0; a < nw->h[i].ncheck; a++) {
                mon_check_t *c = &nw->h[i].c[a];
                for (int b = 0; b < old->h[j].ncheck; b++) {
                    const mon_check_t *o = &old->h[j].c[b];
                    if (c->kind == o->kind && c->port == o->port && !strcmp(c->url, o->url)) {
                        c->state = o->state; c->fails = o->fails; c->ms = o->ms;
                        c->since = o->since;
                        snprintf(c->err, sizeof(c->err), "%s", o->err);
                    }
                }
            }
        }
    }
}

static time_t s_mtime;
static off_t  s_size = -1;
static bool   s_new_list;              /* load_list() replaced the list */

/* Re-reads the file if it changed (or if forced). True if the list is usable. */
static bool load_list(bool force)
{
    if (!sd_card_mounted()) {
        set_problem("No SD card: the server list lives in dashboard/monitors.json.");
        return false;
    }
    struct stat st;
    if (stat(FILE_PATH, &st) != 0) {
        if (s_size != -1 || force || s_cfg->nhosts) {
            monitor_lock();
            s_cfg->nhosts = s_cfg->ngroups = 0;
            monitor_unlock();
        }
        s_size = -1;
        set_problem("No server list yet: tap " LV_SYMBOL_EDIT " to add servers, or copy "
                    "monitors.json to the card's dashboard/ folder (see the README).");
        return false;
    }
    if (!force && st.st_mtime == s_mtime && st.st_size == s_size) return s_cfg->nhosts > 0;
    s_mtime = st.st_mtime;
    s_size = st.st_size;
    if (st.st_size <= 0 || st.st_size > FILE_MAX) {
        set_problem("monitors.json is empty or over 64 KB.");
        return false;
    }
    char *text = heap_caps_calloc(1, st.st_size + 1, MALLOC_CAP_SPIRAM);
    mon_cfg_t *nw = heap_caps_malloc(sizeof(mon_cfg_t), MALLOC_CAP_SPIRAM);
    FILE *f = text && nw ? fopen(FILE_PATH, "rb") : NULL;
    bool ok = f && fread(text, 1, st.st_size, f) == (size_t)st.st_size;
    if (f) fclose(f);
    char why[96] = "Couldn't read monitors.json.";
    if (ok) ok = parse_list(text, nw, why, sizeof(why));
    if (ok) {
        monitor_lock();
        carry_over(nw, s_cfg);
        memcpy(s_cfg, nw, sizeof(*nw));
        s_problem[0] = '\0';
        monitor_unlock();
        s_new_list = true;
        ESP_LOGI(TAG, "%d hosts in %d groups, every %d s", nw->nhosts, nw->ngroups, nw->interval);
        bump();
    } else {
        set_problem(why);           /* the old list (if any) keeps running */
        ESP_LOGW(TAG, "%s", why);
    }
    free(text);
    free(nw);
    return ok || s_cfg->nhosts > 0;
}

/* JSON string with the needed escapes (the on-screen keyboard only types
 * ASCII, but a file from the PC may hold anything). */
static void put_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = *s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

/* The check as parse_check() reads it. */
static void check_text(char *out, size_t len, const mon_check_t *c)
{
    if (c->kind == MON_PING) snprintf(out, len, "ping");
    else if (c->kind == MON_TCP) snprintf(out, len, "tcp %u%s%s", c->port, c->label[0] ? " " : "", c->label);
    else snprintf(out, len, "http %s%s%s", c->url, c->label[0] ? " " : "", c->label);
}

bool monitor_save(const mon_cfg_t *cfg, char *why, size_t wl)
{
    if (!sd_card_mounted()) {
        snprintf(why, wl, "No SD card.");
        return false;
    }
    FILE *f = fopen(FILE_TMP, "w");
    if (!f) {
        snprintf(why, wl, "Couldn't create a file on the SD card.");
        return false;
    }
    /* Same layout as monitor/monitors.example.json. */
    fprintf(f, "{\n \"format\": \"dashboard-monitor\",\n \"version\": 1,\n \"interval\": %d,\n"
            " \"groups\": [", cfg->interval ? cfg->interval : 60);
    for (int g = 0; g < cfg->ngroups; g++) {
        fprintf(f, "%s\n  {\n   \"name\": ", g ? "," : "");
        put_str(f, cfg->group[g]);
        fprintf(f, ",\n   \"hosts\": [");
        bool first = true;
        for (int i = 0; i < cfg->nhosts; i++) {
            const mon_host_t *h = &cfg->h[i];
            if (h->group != g) continue;
            fprintf(f, "%s\n    {\n     \"name\": ", first ? "" : ",");
            first = false;
            put_str(f, h->name);
            fprintf(f, ",\n     \"host\": ");
            put_str(f, h->addr);
            fprintf(f, ",\n     \"what\": ");
            put_str(f, h->what);
            fprintf(f, ",\n     \"checks\": [");
            for (int k = 0; k < h->ncheck; k++) {
                char t[200];
                check_text(t, sizeof(t), &h->c[k]);
                fprintf(f, "%s\n      ", k ? "," : "");
                put_str(f, t);
            }
            fprintf(f, "\n     ]\n    }");
        }
        fprintf(f, "%s]\n  }", first ? "" : "\n   ");
    }
    fprintf(f, "\n ]\n}\n");
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(FILE_TMP);
        snprintf(why, wl, "Couldn't write to the SD card (full?).");
        return false;
    }
    /* FAT can't rename over a file: old -> .bak, then .tmp -> the list. */
    unlink(FILE_BAK);
    rename(FILE_PATH, FILE_BAK);
    if (rename(FILE_TMP, FILE_PATH) != 0) {
        snprintf(why, wl, "Couldn't rename monitors.json.tmp on the SD card.");
        return false;
    }
    ESP_LOGI(TAG, "Saved %d hosts in %d groups", cfg->nhosts, cfg->ngroups);
    __atomic_store_n(&s_save_pending, true, __ATOMIC_RELEASE);
    monitor_reload();
    return true;
}

bool monitor_save_pending(void) { return __atomic_load_n(&s_save_pending, __ATOMIC_ACQUIRE); }

/* ======================= checks ======================= */

static bool resolve(const char *host, struct in_addr *ip)
{
    if (inet_aton(host, ip)) return true;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    *ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static uint16_t csum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (int i = 0; i + 1 < len; i += 2) sum += (p[i] << 8) | p[i + 1];
    if (len & 1) sum += p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

static bool ping_once(struct in_addr ip, int *ms, char *err, size_t el)
{
    static uint16_t seq;
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) {
        snprintf(err, el, "no socket");
        return false;
    }
    uint8_t pkt[24] = { 8, 0 };                     /* echo request */
    uint16_t id = (uint16_t)(uintptr_t)xTaskGetCurrentTaskHandle();
    uint16_t sq = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    pkt[4] = id >> 8; pkt[5] = id; pkt[6] = sq >> 8; pkt[7] = sq;
    memcpy(pkt + 8, "dashboard-monitor", 16);
    uint16_t c = csum(pkt, sizeof(pkt));
    memcpy(pkt + 2, &c, 2);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_addr = ip };
    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    if (sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) > 0) {
        for (;;) {
            int left = PING_MS - (int)((esp_timer_get_time() - t0) / 1000);
            if (left <= 0) break;
            struct timeval tv = { .tv_sec = left / 1000, .tv_usec = (left % 1000) * 1000 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            uint8_t buf[64];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) break;
            int ihl = (buf[0] & 0x0F) * 4;
            if (n < ihl + 8 || from.sin_addr.s_addr != ip.s_addr) continue;
            const uint8_t *r = buf + ihl;
            if (r[0] == 0 && ((r[4] << 8) | r[5]) == id && ((r[6] << 8) | r[7]) == sq) {
                ok = true;
                break;
            }
        }
    }
    close(s);
    *ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (!ok) snprintf(err, el, "no reply");
    return ok;
}

static void sock_err(char *err, size_t el, int e)
{
    const char *t = e == ECONNREFUSED ? "refused" : e == ECONNRESET ? "connection reset" :
                    e == EHOSTUNREACH ? "unreachable" : e == ETIMEDOUT ? "timeout" :
                    e == ENETUNREACH ? "no route" : NULL;
    if (t) snprintf(err, el, "%s", t);
    else snprintf(err, el, "error %d", e);
}

/* Two tries: a sleeping Wi-Fi client can miss the first one. */
static bool do_ping(struct in_addr ip, int *ms, char *err, size_t el)
{
    return ping_once(ip, ms, err, el) || ping_once(ip, ms, err, el);
}

static bool do_tcp(struct in_addr ip, int port, int *ms, char *err, size_t el)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        snprintf(err, el, "no socket");
        return false;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = ip };
    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    int r = connect(s, (struct sockaddr *)&to, sizeof(to));
    if (r == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = { .tv_sec = TCP_MS / 1000, .tv_usec = (TCP_MS % 1000) * 1000 };
        if (select(s + 1, NULL, &w, NULL, &tv) > 0) {
            int e = 0;
            socklen_t l = sizeof(e);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &l);
            ok = e == 0;
            if (!ok) sock_err(err, el, e);
        } else {
            snprintf(err, el, "timeout");
        }
    } else {
        sock_err(err, el, errno);
    }
    *ms = (int)((esp_timer_get_time() - t0) / 1000);
    close(s);
    return ok;
}

static bool do_http(const char *url, int *ms, char *err, size_t el)
{
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = HTTP_MS, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, el, "no memory");
        return false;
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    /* Only the status line matters: open, read the headers, close. */
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        int st = esp_http_client_get_status_code(c);
        ok = st > 0 && st < 500;
        if (!ok) snprintf(err, el, "HTTP %d", st);
    } else {
        snprintf(err, el, "no answer");
    }
    *ms = (int)((esp_timer_get_time() - t0) / 1000);
    esp_http_client_cleanup(c);
    return ok;
}

/* Checks one host. The list can't change during a round (the coordinator
 * only reloads between rounds), so reading it without the lock is safe;
 * results are written under the lock. */
static void check_host(int i)
{
    const mon_host_t *h = &s_cfg->h[i];
    struct in_addr ip;
    bool have_ip = resolve(h->addr, &ip);
    for (int k = 0; k < h->ncheck; k++) {
        const mon_check_t *c = &h->c[k];
        int ms = -1;
        char err[32] = "";
        bool ok = false;
        if (c->kind == MON_HTTP) ok = do_http(c->url, &ms, err, sizeof(err));
        else if (!have_ip) snprintf(err, sizeof(err), "name not found");
        else if (c->kind == MON_PING) ok = do_ping(ip, &ms, err, sizeof(err));
        else ok = do_tcp(ip, c->port, &ms, err, sizeof(err));

        monitor_lock();
        mon_check_t *w = &s_cfg->h[i].c[k];
        mon_state_t was = w->state;
        if (ok) {
            w->fails = 0;
            w->ms = ms;
            w->err[0] = '\0';
            if (was != MON_UP) { w->state = MON_UP; w->since = monitor_now(); }
        } else {
            if (w->fails < 255) w->fails++;
            snprintf(w->err, sizeof(w->err), "%s", err);
            /* Down after two failures in a row (or at once if never seen up). */
            if (was != MON_DOWN && (w->fails >= 2 || was == MON_UNKNOWN)) {
                w->state = MON_DOWN;
                w->since = monitor_now();
            }
        }
        monitor_unlock();
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            int i = __atomic_fetch_add(&s_next, 1, __ATOMIC_ACQ_REL);
            if (i >= s_cfg->nhosts) break;
            check_host(i);
        }
        xSemaphoreGive(s_done);
    }
}

/* ======================= alerts ======================= */

static void push_alert(const char *fmt, const char *name)
{
    monitor_lock();
    snprintf(s_alert[s_alert_w % ALERTS], sizeof(s_alert[0]), fmt, name);
    s_alert_w++;
    if (s_alert_w - s_alert_r > ALERTS) s_alert_r = s_alert_w - ALERTS;
    monitor_unlock();
}

bool monitor_take_alert(char *msg, size_t len)
{
    bool got = false;
    monitor_lock();
    if (s_alert_r != s_alert_w) {
        snprintf(msg, len, "%s", s_alert[s_alert_r % ALERTS]);
        s_alert_r++;
        got = true;
    }
    monitor_unlock();
    return got && s_opt_pop;
}

/* "Has a problem" per host before the round, to spot changes. */
static uint8_t s_prev[MON_HOSTS];          /* 0 unknown, 1 fine, 2 problem */

static void compare_round(bool first)
{
    for (int i = 0; i < s_cfg->nhosts; i++) {
        monitor_lock();
        bool partial;
        mon_state_t st = monitor_host_state(&s_cfg->h[i], &partial);
        monitor_unlock();
        uint8_t now = st == MON_UNKNOWN ? 0 : (st == MON_DOWN || partial) ? 2 : 1;
        if (!first && s_prev[i] == 1 && now == 2) {
            push_alert(st == MON_DOWN ? "%s is down" : "%s has a problem", s_cfg->h[i].name);
        } else if (!first && s_prev[i] == 2 && now == 1) {
            push_alert("%s is back up", s_cfg->h[i].name);
        }
        s_prev[i] = now;
    }
}

/* ======================= coordinator ======================= */

static void coordinator(void *arg)
{
    (void)arg;
    for (int i = 0; i < 60 && !sd_card_mounted(); i++) vTaskDelay(pdMS_TO_TICKS(500));
    for (;;) {
        bool force = __atomic_exchange_n(&s_reload, false, __ATOMIC_ACQ_REL);
        s_new_list = false;
        bool pending = __atomic_load_n(&s_save_pending, __ATOMIC_ACQUIRE);
        bool usable = load_list(force);
        if (pending && force) __atomic_store_n(&s_save_pending, false, __ATOMIC_RELEASE);
        bool first = s_new_list;                    /* new list: no alerts this round */
        if (first) memset(s_prev, 0, sizeof(s_prev));
        int interval = s_cfg->interval ? s_cfg->interval : 60;
        bool no_wifi = usable && !net_time_wifi_connected();
        if (no_wifi) {
            set_problem("Waiting for Wi-Fi...");
        } else if (usable) {
            if (!s_problem[0] || !strcmp(s_problem, "Waiting for Wi-Fi...")) set_problem("");
            __atomic_store_n(&s_next, 0, __ATOMIC_RELEASE);
            for (int k = 0; k < MON_WORKERS; k++) xTaskNotifyGive(s_work[k]);
            for (int k = 0; k < MON_WORKERS; k++) xSemaphoreTake(s_done, portMAX_DELAY);
            s_last_round = monitor_now();
            compare_round(first);
            bump();
        }
        /* Sleep until the next round, waking early for monitor_kick(). The
         * file is checked for changes every 10 s meanwhile. */
        int64_t until = esp_timer_get_time() + (int64_t)interval * 1000000;
        int tick = 0;
        while (esp_timer_get_time() < until) {
            /* Without Wi-Fi: look once a second and start as soon as it's up. */
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(no_wifi ? 1000 : 10000))) break;
            if (no_wifi) {
                if (net_time_wifi_connected()) break;
                if (++tick % 10) continue;          /* the file check below: every 10 s */
            }
            if (__atomic_load_n(&s_reload, __ATOMIC_ACQUIRE)) break;
            struct stat st;
            bool have = sd_card_mounted() && stat(FILE_PATH, &st) == 0;
            if (have != (s_size != -1) || (have && (st.st_mtime != s_mtime || st.st_size != s_size))) break;
        }
    }
}

void monitor_kick(void)   { if (s_coord) xTaskNotifyGive(s_coord); }
void monitor_reload(void) { __atomic_store_n(&s_reload, true, __ATOMIC_RELEASE); monitor_kick(); }

void monitor_init(void)
{
    s_opt_tk = settings_get_u8("mon_tk", 1);
    s_opt_pop = settings_get_u8("mon_pop", 1);
    s_opt_sort = settings_get_u8("mon_sort", 1);
    s_mx = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateCounting(MON_WORKERS, 0);
    s_cfg = heap_caps_calloc(1, sizeof(mon_cfg_t), MALLOC_CAP_SPIRAM);
    if (!s_mx || !s_done || !s_cfg) {
        ESP_LOGE(TAG, "Out of memory");
        return;
    }
    /* No flash writes in these tasks, so PSRAM stacks are fine. */
    for (int k = 0; k < MON_WORKERS; k++) {
        xTaskCreateWithCaps(worker, "mon_chk", 6144, NULL, 2, &s_work[k], MALLOC_CAP_SPIRAM);
    }
    xTaskCreateWithCaps(coordinator, "monitor", 6144, NULL, 2, &s_coord, MALLOC_CAP_SPIRAM);
}

/* ======================= ticker ======================= */

int monitor_ticker_items(char (*out)[TICKER_ITEM_LEN], int max)
{
    if (!s_opt_tk || max < 1) return 0;
    int up = 0, prob = 0, n = 0;
    char down[200] = "";
    monitor_lock();
    for (int i = 0; i < s_cfg->nhosts; i++) {
        bool partial;
        mon_state_t st = monitor_host_state(&s_cfg->h[i], &partial);
        if (st == MON_UP && !partial) up++;
        if (st == MON_DOWN || partial) {
            prob++;
            size_t l = strlen(down);
            if (l < sizeof(down) - 40) {
                snprintf(down + l, sizeof(down) - l, "%s%s", l ? ", " : "", s_cfg->h[i].name);
            } else if (!strstr(down, "...")) {
                snprintf(down + l, sizeof(down) - l, "...");
            }
        }
    }
    bool have = s_cfg->nhosts > 0 && s_last_round;
    monitor_unlock();
    if (!have) return 0;
    if (prob) {
        snprintf(out[n++], TICKER_ITEM_LEN, "#FFD54F Servers#  #66BB6A %d up#   "
                 "#EF5350 " LV_SYMBOL_WARNING " %s#", up, down);
    } else {
        snprintf(out[n++], TICKER_ITEM_LEN, "#FFD54F Servers#  #66BB6A " LV_SYMBOL_OK
                 " all %d up#", up);
    }
    return n;
}
