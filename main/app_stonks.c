/*
 * St0nks: a stock watchlist. Prices come from Yahoo Finance's public chart
 * API (no key needed; quotes are delayed about 15 minutes). Earnings report
 * dates (the quarterly call) come from Nasdaq's public API, US stocks only.
 *
 * Screens (both inside the app's area):
 *   - List: one row per symbol with price, today's change and a sparkline.
 *     "+ Add" opens an on-screen keyboard to add a symbol.
 *   - Detail: tap a row. Big chart with 1D..5Y ranges (touch the chart to
 *     read the price at any point), day / 52-week stats, and Remove.
 *
 * Threading: a worker task (created on first open, then kept) takes jobs
 * {symbol, range} from s_jobs, downloads and parses them, and sends a
 * heap-allocated quote_t* back through s_results. The LVGL timer drains
 * s_results and owns all the data after that. No LVGL calls in the worker.
 *
 * Refreshes: prices every 15 minutes (the refresh button fetches at once),
 * earnings dates every 12 hours.
 *
 * The watchlist is saved in NVS (key "stonks_list", comma separated). The
 * downloaded data stays in PSRAM while the app is closed, so reopening it
 * shows the last prices at once.
 */
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "app_framework.h"
#include "game_common.h"
#include "net_time.h"
#include "settings.h"

static const char *TAG = "stonks";

extern const lv_img_dsc_t img_stonks_icon, img_stonks_small;

#define MAX_SYMS     20
#define SYM_LEN      16
#define MAX_PTS      300
#define LIST_KEY     "stonks_list"
#define DEFAULT_LIST "GME,AMC,SPY,AAPL,NVDA,BTC-USD"
#define REFRESH_US   (15LL * 60 * 1000000)
#define EARN_OK_US   (12LL * 3600 * 1000000)   /* re-check earnings dates */
#define EARN_FAIL_US (60LL * 60 * 1000000)     /* retry after a failed check */

#define C_UP         0x00C853
#define C_DOWN       0xFF5252
#define C_FLAT       0x9AA5B1
#define C_PILL_UP    0x1B8F4A
#define C_PILL_DOWN  0xD32F2F
#define C_PILL_FLAT  0x546E7A
#define C_ACCENT     0x42A5F5
#define C_BUTTON     0x2A333C

enum { R_1D, R_5D, R_1M, R_6M, R_1Y, R_5Y, R_COUNT };
#define R_EARN 100   /* job "range" that fetches earnings dates instead */

static const struct {
    const char *range, *interval, *phrase;
} RANGES[R_COUNT] = {
    { "1d",  "5m",  "today" },
    { "5d",  "15m", "past 5 days" },
    { "1mo", "60m", "past month" },
    { "6mo", "1d",  "past 6 months" },
    { "1y",  "1d",  "past year" },
    { "5y",  "1wk", "past 5 years" },
};
static const char *s_range_map[] = { "1D", "5D", "1M", "6M", "1Y", "5Y", "" };

/* One download: the quote plus chart points for one symbol and range. */
typedef struct {
    char     sym[SYM_LEN];
    int      range;
    bool     ok;
    char     err[40];
    char     name[48];
    char     currency[8];
    char     exchange[24];
    bool     crypto;
    bool     equity;                    /* a company's stock (has earnings) */
    int      earn_next, earn_last;      /* R_EARN jobs: YYYYMMDD, 0 = none */
    double   price, prev_close, chart_prev;
    double   day_hi, day_lo, wk_hi, wk_lo, volume;
    int64_t  mkt_time;
    int64_t  open_start, open_end;      /* current/next regular session */
    int64_t  sess_start, sess_end;      /* session the 1D points belong to */
    int64_t  fetched_us;
    int      npts;
    float    pts[MAX_PTS];
    uint32_t ts[MAX_PTS];
} quote_t;

typedef struct {
    char sym[SYM_LEN];
    int  range;
} job_t;

/* ---------- worker side ---------- */

static QueueHandle_t s_jobs;       /* job_t */
static QueueHandle_t s_results;    /* quote_t * */
static int           s_inflight;   /* jobs queued or running; __atomic */

/* ---------- data (LVGL task only; kept while the app is closed) ---------- */

typedef struct {
    char     sym[SYM_LEN];
    quote_t *q;                    /* latest 1D download, or NULL */
    bool     failed;               /* last refresh failed; q is older data */
    int      earn_next, earn_last; /* earnings report dates, YYYYMMDD or 0 */
    int64_t  earn_due_us;          /* when to check them again; 0 = now */
    bool     earn_pending;
} entry_t;

static struct {
    bool     loaded;
    entry_t  ent[MAX_SYMS];
    int      n;
    int64_t  last_refresh_us;      /* 0 = never */
    time_t   updated;              /* wall time of the last good result */
    char     detail_sym[SYM_LEN];
    int      detail_range;
    quote_t *detail;               /* latest non-1D download */
} D;

/* ---------- UI (only while the app is open) ---------- */

static struct {
    lv_obj_t   *root, *list_view, *detail_view, *add_view;
    lv_obj_t   *status, *list, *empty;
    lv_obj_t   *row[MAX_SYMS];
    lv_obj_t   *r_price[MAX_SYMS], *r_chg[MAX_SYMS], *r_pill[MAX_SYMS],
               *r_pct[MAX_SYMS], *r_name[MAX_SYMS], *r_spark[MAX_SYMS];
    lv_obj_t   *d_sym, *d_name, *d_price, *d_chg, *d_when, *d_chart;
    lv_obj_t   *r_earn[MAX_SYMS];
    lv_obj_t   *d_stat[8], *d_earn_title;
    lv_obj_t   *ta, *add_msg;
    int         scrub;             /* chart point under the finger, or -1 */
    lv_timer_t *timer;
} U;

/* ======================= download + parse (worker task) ======================= */

typedef struct {
    char *buf;
    int   len, cap;
    bool  overflow;
} http_buf_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    http_buf_t *b = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && b) {
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

static double jnum(const cJSON *obj, const char *key, double def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static void jstr(const cJSON *obj, const char *key, char *out, size_t n)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring[0] && !out[0]) {
        snprintf(out, n, "%s", it->valuestring);
    }
}

/* Keeps the name ASCII: the fonts have no other characters. */
static void ascii_only(char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s >= 0x80) *s = '?';
    }
}

static bool parse_chart(const char *json, quote_t *q)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(q->err, sizeof(q->err), "Bad response");
        return false;
    }
    const cJSON *chart = cJSON_GetObjectItemCaseSensitive(root, "chart");
    const cJSON *res = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(chart, "result"), 0);
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(res, "meta");
    if (!meta) {
        snprintf(q->err, sizeof(q->err), "Symbol not found");
        cJSON_Delete(root);
        return false;
    }

    jstr(meta, "longName", q->name, sizeof(q->name));
    jstr(meta, "shortName", q->name, sizeof(q->name));
    ascii_only(q->name);
    jstr(meta, "currency", q->currency, sizeof(q->currency));
    jstr(meta, "fullExchangeName", q->exchange, sizeof(q->exchange));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(meta, "instrumentType");
    q->crypto = cJSON_IsString(type) && strcmp(type->valuestring, "CRYPTOCURRENCY") == 0;
    q->equity = cJSON_IsString(type) && strcmp(type->valuestring, "EQUITY") == 0;

    q->price      = jnum(meta, "regularMarketPrice", NAN);
    q->chart_prev = jnum(meta, "chartPreviousClose", NAN);
    q->prev_close = jnum(meta, "previousClose", q->chart_prev);
    q->day_hi     = jnum(meta, "regularMarketDayHigh", NAN);
    q->day_lo     = jnum(meta, "regularMarketDayLow", NAN);
    q->wk_hi      = jnum(meta, "fiftyTwoWeekHigh", NAN);
    q->wk_lo      = jnum(meta, "fiftyTwoWeekLow", NAN);
    q->volume     = jnum(meta, "regularMarketVolume", NAN);
    q->mkt_time   = (int64_t)jnum(meta, "regularMarketTime", 0);

    const cJSON *reg = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(meta, "currentTradingPeriod"), "regular");
    q->open_start = (int64_t)jnum(reg, "start", 0);
    q->open_end   = (int64_t)jnum(reg, "end", 0);

    /* 1D: "tradingPeriods": [[{start, end}]] is the session of the points. */
    const cJSON *tp = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(meta, "tradingPeriods"), 0);
    if (cJSON_IsArray(tp)) tp = cJSON_GetArrayItem(tp, 0);
    q->sess_start = (int64_t)jnum(tp, "start", 0);
    q->sess_end   = (int64_t)jnum(tp, "end", 0);

    /* Points: skip nulls; if there are too many, keep an even spread. */
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(res, "timestamp");
    const cJSON *quote = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(res, "indicators"), "quote"), 0);
    const cJSON *close = cJSON_GetObjectItemCaseSensitive(quote, "close");
    int total = cJSON_GetArraySize(ts);
    if (cJSON_GetArraySize(close) < total) total = cJSON_GetArraySize(close);
    int step = total > MAX_PTS ? (total + MAX_PTS - 1) / MAX_PTS : 1;
    const cJSON *t = ts ? ts->child : NULL, *c = close ? close->child : NULL;
    for (int i = 0; i < total && t && c; i++, t = t->next, c = c->next) {
        if (i % step != 0 && i != total - 1) continue;
        if (!cJSON_IsNumber(c) || q->npts >= MAX_PTS) continue;
        q->pts[q->npts] = (float)c->valuedouble;
        q->ts[q->npts] = (uint32_t)t->valuedouble;
        q->npts++;
    }
    cJSON_Delete(root);

    if (isnan(q->price) && q->npts > 0) q->price = q->pts[q->npts - 1];
    if (isnan(q->chart_prev)) q->chart_prev = q->npts > 0 ? q->pts[0] : q->price;
    if (isnan(q->prev_close)) q->prev_close = q->chart_prev;
    if (isnan(q->price)) {
        snprintf(q->err, sizeof(q->err), "No price data");
        return false;
    }
    return true;
}

static void url_encode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '.') {
            out[o++] = *in;
        } else {
            o += snprintf(out + o, n - o, "%%%02X", (unsigned char)*in);
        }
    }
    out[o] = '\0';
}

/* Downloads url into hb (hb->buf must have room for cap + 1 bytes).
 * Returns the HTTP status, or -1 if the download itself failed. */
static int http_get(const char *url, http_buf_t *hb)
{
    hb->len = 0;
    hb->overflow = false;
    hb->buf[0] = '\0';
    esp_http_client_config_t cfg = {
        .url               = url,
        .event_handler     = http_event,
        .user_data         = hb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .buffer_size       = 4096,      /* Yahoo sends long headers */
        .buffer_size_tx    = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    /* Yahoo and Nasdaq refuse requests without a browser-like User-Agent. */
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64)");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
        return -1;
    }
    if (hb->overflow) {
        ESP_LOGW(TAG, "%s: response too large", url);
        return -1;
    }
    return status;
}

static void fetch(const job_t *job, quote_t *q, http_buf_t *hb)
{
    char sym[48], url[200];
    url_encode(job->sym, sym, sizeof(sym));
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=%s&interval=%s",
             sym, RANGES[job->range].range, RANGES[job->range].interval);

    int status = http_get(url, hb);
    if (status == 200 || status == 404) {
        q->ok = parse_chart(hb->buf, q);
    } else {
        ESP_LOGW(TAG, "%s: HTTP %d", job->sym, status);
        snprintf(q->err, sizeof(q->err),
                 status == 429 ? "Too many requests" : "Download failed");
    }
}

/* ---------- earnings dates (Nasdaq) ---------- */

static int ymd(int y, int m, int d)
{
    return (y >= 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31) ? y * 10000 + m * 100 + d : 0;
}

/* Today's local date as YYYYMMDD, or 0 if the clock isn't set. */
static int today_ymd(void)
{
    if (!net_time_is_synced()) return 0;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return ymd(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static const char *const MONTHS[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* {"data":{"announcement":"Earnings announcement* for AAPL: Nov 02, 2026"}} */
static int parse_next_earnings(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "data"), "announcement");
    int date = 0;
    const char *p = cJSON_IsString(a) ? strrchr(a->valuestring, ':') : NULL;
    char mon[4];
    int d, y;
    if (p && sscanf(p + 1, " %3s %d, %d", mon, &d, &y) == 3) {
        for (int m = 0; m < 12; m++) {
            if (strcmp(mon, MONTHS[m]) == 0) date = ymd(y, m + 1, d);
        }
    }
    cJSON_Delete(root);
    return date;
}

/* {"data":{"earningsSurpriseTable":{"rows":[{"dateReported":"7/30/2026",..}]}}}
 * The newest report comes first. */
static int parse_last_earnings(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(root, "data"), "earningsSurpriseTable");
    const cJSON *row = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(t, "rows"), 0);
    const cJSON *dr = cJSON_GetObjectItemCaseSensitive(row, "dateReported");
    int date = 0, m, d, y;
    if (cJSON_IsString(dr) && sscanf(dr->valuestring, "%d/%d/%d", &m, &d, &y) == 3) {
        date = ymd(y, m, d);
    }
    cJSON_Delete(root);
    return date;
}

/* The next scheduled report; if there isn't one coming up, the last one. */
static void fetch_earnings(const job_t *job, quote_t *q, http_buf_t *hb)
{
    char sym[48], url[200];
    url_encode(job->sym, sym, sizeof(sym));

    snprintf(url, sizeof(url), "https://api.nasdaq.com/api/analyst/%s/earnings-date", sym);
    int status = http_get(url, hb);
    if (status != 200) {
        ESP_LOGW(TAG, "%s earnings: HTTP %d", job->sym, status);
        return;
    }
    q->earn_next = parse_next_earnings(hb->buf);
    int today = today_ymd();
    if (q->earn_next && today && q->earn_next >= today) {
        q->ok = true;
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    snprintf(url, sizeof(url), "https://api.nasdaq.com/api/company/%s/earnings-surprise", sym);
    status = http_get(url, hb);
    if (status != 200) {
        ESP_LOGW(TAG, "%s earnings: HTTP %d", job->sym, status);
        return;
    }
    q->earn_last = parse_last_earnings(hb->buf);
    q->ok = true;
}

static void *psram_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void worker_task(void *arg)
{
    (void)arg;
    /* A year of daily points is ~1500 JSON values; keep cJSON's nodes in
     * PSRAM. This is global, but harmless: free() handles both kinds of
     * memory, so other cJSON users (Weather) keep working. */
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    http_buf_t hb = { .cap = 64 * 1024 };
    hb.buf = heap_caps_malloc(hb.cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    job_t job;
    for (;;) {
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        quote_t *q = heap_caps_calloc(1, sizeof(quote_t), MALLOC_CAP_SPIRAM);
        if (q) {
            memcpy(q->sym, job.sym, sizeof(q->sym));
            q->range = job.range;
            if (!hb.buf) {
                snprintf(q->err, sizeof(q->err), "Out of memory");
            } else if (job.range == R_EARN) {
                fetch_earnings(&job, q, &hb);
            } else {
                fetch(&job, q, &hb);
            }
            q->fetched_us = esp_timer_get_time();
            xQueueSend(s_results, &q, portMAX_DELAY);
        }
        __atomic_sub_fetch(&s_inflight, 1, __ATOMIC_ACQ_REL);
        vTaskDelay(pdMS_TO_TICKS(100));   /* be gentle with Yahoo */
    }
}

static void worker_init(void)
{
    if (s_jobs) return;
    s_jobs = xQueueCreate(MAX_SYMS + 8, sizeof(job_t));
    s_results = xQueueCreate(MAX_SYMS + 8, sizeof(quote_t *));
    if (xTaskCreateWithCaps(worker_task, "stonks", 10240, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Could not start download task");
    }
}

static int inflight(void) { return __atomic_load_n(&s_inflight, __ATOMIC_ACQUIRE); }

/* Queues a download; false if it couldn't be queued. */
static bool request(const char *sym, int range)
{
    if (!s_jobs || !net_time_wifi_connected()) return false;
    job_t job = { .range = range };
    snprintf(job.sym, sizeof(job.sym), "%s", sym);
    __atomic_add_fetch(&s_inflight, 1, __ATOMIC_ACQ_REL);
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        __atomic_sub_fetch(&s_inflight, 1, __ATOMIC_ACQ_REL);
        return false;
    }
    return true;
}

static void refresh_all(void)
{
    if (inflight() > 0 || !net_time_wifi_connected()) return;
    D.last_refresh_us = esp_timer_get_time();
    for (int i = 0; i < D.n; i++) request(D.ent[i].sym, R_1D);
}

/* ======================= watchlist ======================= */

static void load_list(void)
{
    char buf[MAX_SYMS * SYM_LEN];
    if (!settings_get_str(LIST_KEY, buf, sizeof(buf))) {
        snprintf(buf, sizeof(buf), "%s", DEFAULT_LIST);
    }
    D.n = 0;
    for (char *s = strtok(buf, ","); s && D.n < MAX_SYMS; s = strtok(NULL, ",")) {
        snprintf(D.ent[D.n++].sym, SYM_LEN, "%s", s);
    }
    D.loaded = true;
}

static void save_list(void)
{
    char buf[MAX_SYMS * SYM_LEN] = "";
    for (int i = 0; i < D.n; i++) {
        if (i) strlcat(buf, ",", sizeof(buf));
        strlcat(buf, D.ent[i].sym, sizeof(buf));
    }
    settings_set_str(LIST_KEY, buf);
}

static int find_sym(const char *sym)
{
    for (int i = 0; i < D.n; i++) {
        if (strcmp(D.ent[i].sym, sym) == 0) return i;
    }
    return -1;
}

/* ======================= formatting ======================= */

/* 1234.5 -> "1,234.50"; small prices get 4 decimals. */
static void fmt_price(char *out, size_t n, double v)
{
    if (isnan(v)) {
        snprintf(out, n, "--");
        return;
    }
    int dec = fabs(v) < 1.0 ? 4 : 2;
    char raw[32];
    snprintf(raw, sizeof(raw), "%.*f", dec, fabs(v));
    int int_len = (int)(strchr(raw, '.') - raw);
    size_t o = 0;
    if (v < 0 && o + 1 < n) out[o++] = '-';
    for (int i = 0; raw[i] && o + 1 < n; i++) {
        if (i > 0 && i < int_len && (int_len - i) % 3 == 0) out[o++] = ',';
        if (o + 1 < n) out[o++] = raw[i];
    }
    out[o] = '\0';
}

static void fmt_money(char *out, size_t n, double v, const char *currency)
{
    char p[32];
    fmt_price(p, sizeof(p), v);
    if (!currency[0] || strcmp(currency, "USD") == 0) {
        snprintf(out, n, "$%s", p);
    } else {
        snprintf(out, n, "%s %s", p, currency);
    }
}

static void fmt_big(char *out, size_t n, double v)
{
    if (isnan(v))        snprintf(out, n, "--");
    else if (v >= 1e12)  snprintf(out, n, "%.2fT", v / 1e12);
    else if (v >= 1e9)   snprintf(out, n, "%.2fB", v / 1e9);
    else if (v >= 1e6)   snprintf(out, n, "%.2fM", v / 1e6);
    else if (v >= 1e3)   snprintf(out, n, "%.1fK", v / 1e3);
    else                 snprintf(out, n, "%.0f", v);
}

/* "+1.23 (+4.56%)" */
static void fmt_change(char *out, size_t n, double now, double base)
{
    if (isnan(now) || isnan(base) || base == 0) {
        snprintf(out, n, "--");
        return;
    }
    char p[32];
    fmt_price(p, sizeof(p), fabs(now - base));
    snprintf(out, n, "%c%s (%+.2f%%)", now >= base ? '+' : '-', p,
             (now - base) / base * 100.0);
}

static uint32_t change_color(double now, double base)
{
    if (isnan(now) || isnan(base) || now == base) return C_FLAT;
    return now > base ? C_UP : C_DOWN;
}

static void fmt_clock(char *out, size_t n, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    if (settings_get_24h()) {
        strftime(out, n, "%H:%M", &tm);
    } else {
        char b[16];
        strftime(b, sizeof(b), "%I:%M %p", &tm);
        snprintf(out, n, "%s", b[0] == '0' ? b + 1 : b);
    }
}

/* Label for a chart point's time, suited to the range. */
static void fmt_point_time(char *out, size_t n, time_t t, int range)
{
    struct tm tm;
    localtime_r(&t, &tm);
    char clock[16];
    fmt_clock(clock, sizeof(clock), t);
    switch (range) {
    case R_1D: snprintf(out, n, "%s", clock); break;
    case R_5D:
    case R_1M: {
        char d[16];
        strftime(d, sizeof(d), "%a %b %d", &tm);
        snprintf(out, n, "%s, %s", d, clock);
        break;
    }
    default:   strftime(out, n, "%b %d, %Y", &tm); break;
    }
}

/* true/false = open/closed; -1 = unknown (no clock or no session info). */
static int market_open(const quote_t *q)
{
    if (!q || !q->ok) return -1;
    if (q->crypto) return 1;
    if (!net_time_is_synced() || q->open_end == 0) return -1;
    time_t now = time(NULL);
    return now >= q->open_start && now < q->open_end;
}

/* The earnings date to show: the next report if one is scheduled, else the
 * last one. Returns the date (YYYYMMDD, 0 = nothing known). */
static int earnings_date(const entry_t *en, bool *upcoming)
{
    int today = today_ymd();
    *upcoming = en->earn_next && (!today || en->earn_next >= today);
    if (*upcoming) return en->earn_next;
    /* A "next" date that has passed is the latest report too. */
    return en->earn_next > en->earn_last ? en->earn_next : en->earn_last;
}

/* "Nov 2" or, with the year, "Nov 2, 2026". */
static void fmt_ymd(char *out, size_t n, int date, bool year)
{
    int y = date / 10000, m = date / 100 % 100, d = date % 100;
    if (year) snprintf(out, n, "%s %d, %d", MONTHS[m - 1], d, y);
    else      snprintf(out, n, "%s %d", MONTHS[m - 1], d);
}

/* ======================= charts ======================= */

typedef struct {
    lv_area_t plot;
    double    lo, hi;
    bool      by_time;
    int64_t   t0, t1;
} chart_map_t;

static void chart_map(const quote_t *q, const lv_area_t *plot, double base, bool by_time,
                      chart_map_t *m)
{
    m->plot = *plot;
    m->lo = m->hi = q->pts[0];
    for (int i = 1; i < q->npts; i++) {
        if (q->pts[i] < m->lo) m->lo = q->pts[i];
        if (q->pts[i] > m->hi) m->hi = q->pts[i];
    }
    if (!isnan(base)) {
        if (base < m->lo) m->lo = base;
        if (base > m->hi) m->hi = base;
    }
    double pad = (m->hi - m->lo) * 0.06;
    if (pad <= 0) pad = fabs(m->hi) * 0.01 + 0.01;
    m->lo -= pad;
    m->hi += pad;
    /* 1D stock charts span the whole session, so a chart at noon is half
     * full; other charts spread the points evenly (no gaps for nights). */
    m->by_time = by_time && q->sess_end > q->sess_start;
    m->t0 = q->sess_start;
    m->t1 = q->sess_end;
}

static lv_coord_t map_x(const chart_map_t *m, const quote_t *q, int i)
{
    int w = lv_area_get_width(&m->plot) - 1;
    if (m->by_time) {
        int64_t t = q->ts[i];
        if (t < m->t0) t = m->t0;
        if (t > m->t1) t = m->t1;
        return m->plot.x1 + (lv_coord_t)((t - m->t0) * w / (m->t1 - m->t0));
    }
    return m->plot.x1 + (q->npts > 1 ? (lv_coord_t)((int64_t)i * w / (q->npts - 1)) : 0);
}

static lv_coord_t map_y(const chart_map_t *m, double v)
{
    int h = lv_area_get_height(&m->plot) - 1;
    return m->plot.y2 - (lv_coord_t)((v - m->lo) / (m->hi - m->lo) * h);
}

static void draw_series(lv_draw_ctx_t *ctx, const quote_t *q, const chart_map_t *m,
                        uint32_t color, int width, bool fill)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(color);
    ld.width = width;
    ld.round_start = ld.round_end = 1;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = ld.color;
    rd.bg_opa = LV_OPA_20;

    /* Skip points that land on the same pixel column (keeps it fast). */
    lv_point_t prev = { map_x(m, q, 0), map_y(m, q->pts[0]) };
    for (int i = 1; i < q->npts; i++) {
        lv_point_t p = { map_x(m, q, i), map_y(m, q->pts[i]) };
        if (p.x == prev.x && i != q->npts - 1) continue;
        if (fill) {
            for (lv_coord_t x = prev.x; x < p.x; x++) {
                lv_coord_t y = prev.y + (p.y - prev.y) * (x - prev.x) / (p.x - prev.x);
                lv_area_t col = { x, y, x, m->plot.y2 };
                lv_draw_rect(ctx, &rd, &col);
            }
        }
        lv_draw_line(ctx, &ld, &prev, &p);
        prev = p;
    }
}

static void draw_dashed(lv_draw_ctx_t *ctx, const chart_map_t *m, double v)
{
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = lv_color_hex(0x5F6B77);
    ld.width = 1;
    ld.dash_width = 4;
    ld.dash_gap = 4;
    lv_coord_t y = map_y(m, v);
    lv_point_t a = { m->plot.x1, y }, b = { m->plot.x2, y };
    lv_draw_line(ctx, &ld, &a, &b);
}

/* Sparkline in a list row; the row index is the object's user data. */
static void spark_draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(obj);
    if (i >= D.n) return;
    const quote_t *q = D.ent[i].q;
    if (!q || !q->ok || q->npts < 2) return;

    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    chart_map_t m;
    chart_map(q, &a, q->prev_close, !q->crypto, &m);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    draw_dashed(ctx, &m, q->prev_close);
    draw_series(ctx, q, &m, change_color(q->price, q->prev_close), 2, false);
}

/* ======================= detail view ======================= */

static const quote_t *list_quote(void)
{
    int i = find_sym(D.detail_sym);
    return i >= 0 ? D.ent[i].q : NULL;
}

/* The chart data for the selected range, or NULL while loading. */
static const quote_t *detail_quote(void)
{
    if (D.detail_range == R_1D) return list_quote();
    if (D.detail && strcmp(D.detail->sym, D.detail_sym) == 0 &&
        D.detail->range == D.detail_range) {
        return D.detail;
    }
    return NULL;
}

static double chart_base(const quote_t *q)
{
    return D.detail_range == R_1D ? q->prev_close : q->chart_prev;
}

static void chart_area(const lv_area_t *coords, lv_area_t *plot)
{
    plot->x1 = coords->x1 + 8;
    plot->x2 = coords->x2 - 8;
    plot->y1 = coords->y1 + 24;
    plot->y2 = coords->y2 - 26;
}

static void draw_text(lv_draw_ctx_t *ctx, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                      lv_text_align_t align, const char *txt)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(APP_COLOR_SUBTEXT);
    d.font = LV_FONT_DEFAULT;
    d.align = align;
    lv_area_t a = { x, y, x + w - 1, y + 18 };
    lv_draw_label(ctx, &d, &a, txt, NULL);
}

static void detail_chart_draw(lv_event_t *e)
{
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t a, plot;
    lv_obj_get_coords(U.d_chart, &a);
    chart_area(&a, &plot);

    const quote_t *q = detail_quote();
    if (!q || !q->ok || q->npts < 2) {
        const char *msg = !q ? (inflight() ? "Loading..." : "No data yet")
                             : q->ok ? "No chart data" : q->err;
        draw_text(ctx, a.x1, (a.y1 + a.y2) / 2 - 9, lv_area_get_width(&a),
                  LV_TEXT_ALIGN_CENTER, msg);
        return;
    }

    double base = chart_base(q);
    chart_map_t m;
    chart_map(q, &plot, D.detail_range == R_1D ? base : NAN,
              D.detail_range == R_1D && !q->crypto, &m);

    if (D.detail_range == R_1D) draw_dashed(ctx, &m, base);
    draw_series(ctx, q, &m, change_color(q->pts[q->npts - 1], base), 3, true);

    /* Highest / lowest price, and the first / last time. */
    float lo = q->pts[0], hi = q->pts[0];
    for (int i = 1; i < q->npts; i++) {
        lo = fminf(lo, q->pts[i]);
        hi = fmaxf(hi, q->pts[i]);
    }
    char s[40], p[32];
    int w = lv_area_get_width(&plot);
    fmt_price(p, sizeof(p), hi);
    snprintf(s, sizeof(s), "High %s", p);
    draw_text(ctx, plot.x1, a.y1 + 2, w, LV_TEXT_ALIGN_RIGHT, s);
    fmt_price(p, sizeof(p), lo);
    snprintf(s, sizeof(s), "Low %s", p);
    draw_text(ctx, plot.x1, plot.y2 + 4, w, LV_TEXT_ALIGN_RIGHT, s);
    time_t t0 = m.by_time ? (time_t)m.t0 : (time_t)q->ts[0];
    fmt_point_time(s, sizeof(s), t0, D.detail_range);
    draw_text(ctx, plot.x1, plot.y2 + 4, w / 2, LV_TEXT_ALIGN_LEFT, s);

    /* Finger on the chart: a line and a dot on that point. */
    if (U.scrub >= 0 && U.scrub < q->npts) {
        lv_coord_t x = map_x(&m, q, U.scrub), y = map_y(&m, q->pts[U.scrub]);
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = lv_color_hex(0xCFD8DC);
        ld.width = 1;
        lv_point_t p1 = { x, plot.y1 }, p2 = { x, plot.y2 };
        lv_draw_line(ctx, &ld, &p1, &p2);
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.radius = LV_RADIUS_CIRCLE;
        rd.bg_color = lv_color_white();
        rd.border_color = lv_color_hex(change_color(q->pts[U.scrub], base));
        rd.border_width = 3;
        lv_area_t dot = { x - 6, y - 6, x + 6, y + 6 };
        lv_draw_rect(ctx, &rd, &dot);
    }
}

static void update_detail(void)
{
    if (!U.detail_view) return;
    const quote_t *lq = list_quote();
    const quote_t *q = detail_quote();
    const quote_t *info = lq ? lq : q;     /* stats come from the 1D data */
    char buf[64], p[32];

    lv_label_set_text(U.d_sym, D.detail_sym);
    lv_label_set_text(U.d_name, info ? (info->ok ? info->name : info->err) : "Loading...");

    if (q && q->ok && U.scrub >= 0 && U.scrub < q->npts) {
        double v = q->pts[U.scrub], base = chart_base(q);
        fmt_money(buf, sizeof(buf), v, q->currency);
        lv_label_set_text(U.d_price, buf);
        fmt_change(buf, sizeof(buf), v, base);
        lv_label_set_text(U.d_chg, buf);
        lv_obj_set_style_text_color(U.d_chg, lv_color_hex(change_color(v, base)), 0);
        fmt_point_time(buf, sizeof(buf), q->ts[U.scrub], D.detail_range);
        lv_label_set_text(U.d_when, buf);
    } else if (q && q->ok) {
        double base = chart_base(q);
        fmt_money(buf, sizeof(buf), q->price, q->currency);
        lv_label_set_text(U.d_price, buf);
        fmt_change(p, sizeof(p), q->price, base);
        snprintf(buf, sizeof(buf), "%s %s", p, RANGES[D.detail_range].phrase);
        lv_label_set_text(U.d_chg, buf);
        lv_obj_set_style_text_color(U.d_chg, lv_color_hex(change_color(q->price, base)), 0);
        int open = market_open(info);
        const char *state = open == 1 ? (q->crypto ? "Trades 24/7" : "Market open")
                          : open == 0 ? "Market closed" : NULL;
        char when[24] = "";
        if (q->mkt_time) fmt_clock(when, sizeof(when), (time_t)q->mkt_time);
        if (state && when[0]) snprintf(buf, sizeof(buf), "%s - as of %s", state, when);
        else if (when[0])     snprintf(buf, sizeof(buf), "As of %s", when);
        else                  snprintf(buf, sizeof(buf), "%s", state ? state : "");
        lv_label_set_text(U.d_when, buf);
    } else if (info && info->ok) {
        fmt_money(buf, sizeof(buf), info->price, info->currency);
        lv_label_set_text(U.d_price, buf);
        lv_label_set_text(U.d_chg, "");
        lv_label_set_text(U.d_when, "");
    } else {
        lv_label_set_text(U.d_price, "--");
        lv_label_set_text(U.d_chg, "");
        lv_label_set_text(U.d_when, "");
    }

    /* Stats panel. */
    const char *cur = info && info->ok ? info->currency : "";
    double vals[5] = { NAN, NAN, NAN, NAN, NAN };
    if (info && info->ok) {
        vals[0] = info->prev_close;
        vals[1] = info->day_hi;
        vals[2] = info->day_lo;
        vals[3] = info->wk_hi;
        vals[4] = info->wk_lo;
    }
    for (int i = 0; i < 5; i++) {
        if (isnan(vals[i])) {
            lv_label_set_text(U.d_stat[i], "--");
        } else {
            fmt_money(buf, sizeof(buf), vals[i], cur);
            lv_label_set_text(U.d_stat[i], buf);
        }
    }
    fmt_big(buf, sizeof(buf), info && info->ok ? info->volume : NAN);
    lv_label_set_text(U.d_stat[5], buf);
    lv_label_set_text(U.d_stat[6], info && info->ok && info->exchange[0] ? info->exchange : "--");

    int ei = find_sym(D.detail_sym);
    bool upcoming = false;
    int edate = ei >= 0 ? earnings_date(&D.ent[ei], &upcoming) : 0;
    if (edate) {
        fmt_ymd(buf, sizeof(buf), edate, true);
        if (upcoming && edate == today_ymd()) strlcat(buf, " (today)", sizeof(buf));
        lv_label_set_text(U.d_earn_title, upcoming ? "Next earnings call" : "Last earnings call");
        lv_label_set_text(U.d_stat[7], buf);
    } else {
        lv_label_set_text(U.d_earn_title, "Earnings call");
        bool waiting = ei >= 0 && info && info->ok && info->equity &&
                       (D.ent[ei].earn_pending || D.ent[ei].earn_due_us == 0);
        lv_label_set_text(U.d_stat[7], waiting ? "Checking..." : "--");
    }

    lv_obj_invalidate(U.d_chart);
}

static void request_detail(void)
{
    if (D.detail_range == R_1D) return;    /* the list refresh covers 1D */
    const quote_t *q = detail_quote();
    if (q && esp_timer_get_time() - q->fetched_us < REFRESH_US) return;
    request(D.detail_sym, D.detail_range);
}

static void chart_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const quote_t *q = detail_quote();
    int old = U.scrub;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || !q || !q->ok ||
        q->npts < 2) {
        U.scrub = -1;
    } else {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_get_act(), &pt);
        lv_area_t a, plot;
        lv_obj_get_coords(U.d_chart, &a);
        chart_area(&a, &plot);
        chart_map_t m;
        chart_map(q, &plot, D.detail_range == R_1D ? chart_base(q) : NAN,
                  D.detail_range == R_1D && !q->crypto, &m);
        int best = 0, best_d = INT32_MAX;
        for (int i = 0; i < q->npts; i++) {
            int d = abs(map_x(&m, q, i) - pt.x);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        U.scrub = best;
    }
    if (U.scrub != old) update_detail();
}

static void show_list(void);

static void back_to_list(lv_event_t *e)
{
    (void)e;
    show_list();
}

static void range_changed(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id >= R_COUNT) return;
    D.detail_range = id;
    U.scrub = -1;
    request_detail();
    update_detail();
}

static void remove_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (!yes) return;
    int i = find_sym(D.detail_sym);
    if (i >= 0) {
        free(D.ent[i].q);
        memmove(&D.ent[i], &D.ent[i + 1], sizeof(entry_t) * (D.n - i - 1));
        D.n--;
        memset(&D.ent[D.n], 0, sizeof(entry_t));
        save_list();
    }
    show_list();
}

static void remove_clicked(lv_event_t *e)
{
    (void)e;
    char title[48];
    snprintf(title, sizeof(title), "Remove %s from the list?", D.detail_sym);
    lv_obj_t *row = game_dialog_open(U.root, title);
    game_dialog_button(row, LV_SYMBOL_TRASH "  Remove", remove_answer, (void *)1);
    game_dialog_button(row, "Cancel", remove_answer, NULL);
}

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *p, const char *text, int w, int h, lv_event_cb_t cb,
                           void *user_data)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_BUTTON), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *mk_view(void)
{
    lv_obj_t *v = lv_obj_create(U.root);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    return v;
}

static void build_detail(void)
{
    lv_obj_t *v = U.detail_view = mk_view();

    lv_obj_t *back = mk_button(v, LV_SYMBOL_LEFT "  List", 110, 44, back_to_list, NULL);
    lv_obj_set_pos(back, 12, 8);

    U.d_sym = mk_label(v, APP_FONT_LARGE, APP_COLOR_TEXT);
    lv_obj_set_pos(U.d_sym, 136, 4);
    U.d_name = mk_label(v, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT);
    lv_label_set_long_mode(U.d_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.d_name, 400);
    lv_obj_set_pos(U.d_name, 136, 38);

    U.d_price = mk_label(v, APP_FONT_HUGE, APP_COLOR_TEXT);
    lv_obj_set_pos(U.d_price, 12, 60);
    U.d_chg = mk_label(v, APP_FONT_MEDIUM, C_FLAT);
    lv_obj_set_pos(U.d_chg, 12, 118);
    U.d_when = mk_label(v, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT);
    lv_label_set_long_mode(U.d_when, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.d_when, 230);
    lv_obj_set_style_text_align(U.d_when, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(U.d_when, 306, 124);

    U.d_chart = lv_obj_create(v);
    lv_obj_remove_style_all(U.d_chart);
    lv_obj_set_size(U.d_chart, 524, 206);
    lv_obj_set_pos(U.d_chart, 12, 148);
    lv_obj_set_style_radius(U.d_chart, 12, 0);
    lv_obj_set_style_bg_color(U.d_chart, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(U.d_chart, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.d_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(U.d_chart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(U.d_chart, detail_chart_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(U.d_chart, chart_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(U.d_chart, chart_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(U.d_chart, chart_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(U.d_chart, chart_touch, LV_EVENT_PRESS_LOST, NULL);

    lv_obj_t *m = lv_btnmatrix_create(v);
    lv_btnmatrix_set_map(m, s_range_map);
    lv_btnmatrix_set_btn_ctrl_all(m, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(m, true);
    lv_btnmatrix_set_btn_ctrl(m, D.detail_range, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(m, 524, 50);
    lv_obj_set_pos(m, 12, 362);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 8, 0);
    lv_obj_set_style_text_font(m, APP_FONT_MEDIUM, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_hex(APP_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 10, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_BUTTON), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, lv_color_hex(0x0D1B2A), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, range_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* Stats panel on the right. */
    static const char *names[8] = { "Prev close", "Day high", "Day low", "52-wk high",
                                    "52-wk low", "Volume", "Exchange", "Earnings call" };
    lv_obj_t *panel = lv_obj_create(v);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 240, 346);
    lv_obj_set_pos(panel, 548, 8);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *n = mk_label(panel, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT);
        lv_label_set_text(n, names[i]);
        lv_obj_set_pos(n, 14, 10 + i * 41);
        U.d_stat[i] = mk_label(panel, APP_FONT_MEDIUM, APP_COLOR_TEXT);
        lv_label_set_long_mode(U.d_stat[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(U.d_stat[i], 212);
        lv_obj_set_pos(U.d_stat[i], 14, 26 + i * 41);
        if (i == 7) U.d_earn_title = n;
    }

    lv_obj_t *rm = mk_button(v, LV_SYMBOL_TRASH "  Remove", 240, 50, remove_clicked, NULL);
    lv_obj_set_pos(rm, 548, 362);
    lv_obj_set_style_bg_color(rm, lv_color_hex(0x5D1F1F), 0);
}

static void open_detail(int i)
{
    snprintf(D.detail_sym, sizeof(D.detail_sym), "%s", D.ent[i].sym);
    U.scrub = -1;
    if (U.detail_view) lv_obj_del(U.detail_view);
    build_detail();
    lv_obj_add_flag(U.list_view, LV_OBJ_FLAG_HIDDEN);
    request_detail();
    update_detail();
}

/* ======================= list view ======================= */

static void update_row(int i)
{
    if (i >= D.n || !U.row[i]) return;
    const quote_t *q = D.ent[i].q;
    char buf[48];

    if (!q) {
        lv_label_set_text(U.r_name[i], inflight() ? "Loading..." : "No data yet");
        lv_obj_set_style_text_color(U.r_name[i], lv_color_hex(APP_COLOR_SUBTEXT), 0);
        lv_label_set_text(U.r_price[i], "");
        lv_label_set_text(U.r_chg[i], "");
        lv_label_set_text(U.r_pct[i], "--");
        lv_obj_set_style_bg_color(U.r_pill[i], lv_color_hex(C_PILL_FLAT), 0);
    } else if (!q->ok) {
        lv_label_set_text(U.r_name[i], q->err);
        lv_obj_set_style_text_color(U.r_name[i], lv_color_hex(C_DOWN), 0);
        lv_label_set_text(U.r_price[i], "");
        lv_label_set_text(U.r_chg[i], "");
        lv_label_set_text(U.r_pct[i], "--");
        lv_obj_set_style_bg_color(U.r_pill[i], lv_color_hex(C_PILL_FLAT), 0);
    } else {
        lv_label_set_text(U.r_name[i], q->name[0] ? q->name : q->sym);
        lv_obj_set_style_text_color(U.r_name[i], lv_color_hex(APP_COLOR_SUBTEXT), 0);
        fmt_money(buf, sizeof(buf), q->price, q->currency);
        lv_label_set_text(U.r_price[i], buf);
        double d = q->price - q->prev_close;
        char p[32];
        fmt_price(p, sizeof(p), fabs(d));
        snprintf(buf, sizeof(buf), "%c%s%s", d >= 0 ? '+' : '-', p,
                 D.ent[i].failed ? "  (old)" : "");
        lv_label_set_text(U.r_chg[i], buf);
        lv_obj_set_style_text_color(U.r_chg[i], lv_color_hex(change_color(q->price, q->prev_close)), 0);
        if (q->prev_close) {
            snprintf(buf, sizeof(buf), "%+.2f%%", d / q->prev_close * 100.0);
        } else {
            snprintf(buf, sizeof(buf), "--");
        }
        lv_label_set_text(U.r_pct[i], buf);
        uint32_t c = change_color(q->price, q->prev_close);
        lv_obj_set_style_bg_color(U.r_pill[i], lv_color_hex(
            c == C_UP ? C_PILL_UP : c == C_DOWN ? C_PILL_DOWN : C_PILL_FLAT), 0);
    }

    bool upcoming = false;
    int edate = earnings_date(&D.ent[i], &upcoming);
    if (edate) {
        char d[16];
        fmt_ymd(d, sizeof(d), edate, false);
        if (upcoming && edate == today_ymd()) snprintf(buf, sizeof(buf), "Earnings today");
        else snprintf(buf, sizeof(buf), "%s %s", upcoming ? "Earnings" : "Reported", d);
        lv_label_set_text(U.r_earn[i], buf);
        lv_obj_set_style_text_color(U.r_earn[i], lv_color_hex(upcoming ? 0xFFD54F : C_FLAT), 0);
    } else {
        lv_label_set_text(U.r_earn[i], "");
    }
    lv_obj_invalidate(U.r_spark[i]);
}

static void update_status(void)
{
    if (!U.status) return;
    char buf[80], when[16];
    if (!net_time_wifi_connected()) {
        lv_label_set_text(U.status, LV_SYMBOL_WARNING "  No Wi-Fi");
        return;
    }
    if (inflight() > 0) {
        lv_label_set_text(U.status, LV_SYMBOL_REFRESH "  Updating...");
        return;
    }
    int open = -1;
    for (int i = 0; i < D.n && open < 0; i++) {
        if (D.ent[i].q && !D.ent[i].q->crypto) open = market_open(D.ent[i].q);
    }
    if (D.updated && net_time_is_synced()) {
        fmt_clock(when, sizeof(when), D.updated);
        snprintf(buf, sizeof(buf), "%s%sUpdated %s",
                 open == 1 ? "Market open" : open == 0 ? "Market closed" : "",
                 open >= 0 ? "  -  " : "", when);
    } else {
        snprintf(buf, sizeof(buf), "%s", open == 1 ? "Market open" : open == 0 ? "Market closed" : "");
    }
    lv_label_set_text(U.status, buf);
}

static void row_clicked(lv_event_t *e)
{
    open_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_rows(void)
{
    lv_obj_clean(U.list);
    memset(U.row, 0, sizeof(U.row));
    if (D.n == 0) {
        lv_obj_clear_flag(U.empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(U.empty, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < D.n; i++) {
        lv_obj_t *r = U.row[i] = lv_obj_create(U.list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), 72);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(APP_COLOR_PANEL), 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x2C3640), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *sym = mk_label(r, APP_FONT_LARGE, APP_COLOR_TEXT);
        lv_label_set_text(sym, D.ent[i].sym);
        lv_obj_set_pos(sym, 16, 6);
        lv_obj_update_layout(sym);

        /* Earnings date, to the right of the symbol. */
        lv_coord_t ex = 16 + lv_obj_get_width(sym) + 10;
        U.r_earn[i] = mk_label(r, LV_FONT_DEFAULT, C_FLAT);
        lv_label_set_long_mode(U.r_earn[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(U.r_earn[i], LV_MAX(272 - ex, 0));
        lv_obj_set_pos(U.r_earn[i], ex, 16);

        U.r_name[i] = mk_label(r, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT);
        lv_label_set_long_mode(U.r_name[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(U.r_name[i], 250);
        lv_obj_set_pos(U.r_name[i], 16, 44);

        lv_obj_t *s = U.r_spark[i] = lv_obj_create(r);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 160, 52);
        lv_obj_set_pos(s, 280, 10);
        lv_obj_set_user_data(s, (void *)(intptr_t)i);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s, spark_draw, LV_EVENT_DRAW_MAIN, NULL);

        U.r_price[i] = mk_label(r, APP_FONT_LARGE, APP_COLOR_TEXT);
        lv_obj_align(U.r_price[i], LV_ALIGN_TOP_RIGHT, -150, 6);
        U.r_chg[i] = mk_label(r, LV_FONT_DEFAULT, C_FLAT);
        lv_obj_align(U.r_chg[i], LV_ALIGN_TOP_RIGHT, -150, 44);

        lv_obj_t *pill = U.r_pill[i] = lv_obj_create(r);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, 118, 44);
        lv_obj_align(pill, LV_ALIGN_RIGHT_MID, -14, 0);
        lv_obj_set_style_radius(pill, 8, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        U.r_pct[i] = mk_label(pill, APP_FONT_MEDIUM, APP_COLOR_TEXT);
        lv_obj_center(U.r_pct[i]);

        update_row(i);
    }
}

static void show_list(void)
{
    if (U.detail_view) {
        lv_obj_del_async(U.detail_view);   /* we may be inside its event */
        U.detail_view = NULL;
    }
    lv_obj_clear_flag(U.list_view, LV_OBJ_FLAG_HIDDEN);
    U.scrub = -1;
    build_rows();
    update_status();
}

/* ---------- adding a symbol ---------- */

static void close_add(void)
{
    if (U.add_view) {
        lv_obj_del_async(U.add_view);      /* we're inside its event */
        U.add_view = NULL;
        U.ta = U.add_msg = NULL;
    }
}

static void add_confirm(void)
{
    char sym[SYM_LEN];
    snprintf(sym, sizeof(sym), "%s", lv_textarea_get_text(U.ta));
    for (char *c = sym; *c; c++) *c = (char)toupper((unsigned char)*c);
    if (!sym[0]) {
        close_add();
        return;
    }
    if (find_sym(sym) >= 0) {
        lv_label_set_text_fmt(U.add_msg, "%s is already in your list.", sym);
        return;
    }
    if (D.n >= MAX_SYMS) {
        lv_label_set_text_fmt(U.add_msg, "The list is full (%d). Remove one first.", MAX_SYMS);
        return;
    }
    memset(&D.ent[D.n], 0, sizeof(entry_t));
    snprintf(D.ent[D.n].sym, SYM_LEN, "%s", sym);
    D.n++;
    save_list();
    request(sym, R_1D);
    close_add();
    build_rows();
    lv_obj_scroll_to_view(U.row[D.n - 1], LV_ANIM_ON);
}

static void add_kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        add_confirm();
    } else {
        close_add();
    }
}

static void add_btn_event(lv_event_t *e)
{
    if (lv_event_get_user_data(e)) {
        add_confirm();
    } else {
        close_add();
    }
}

static void add_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *v = U.add_view = lv_obj_create(U.root);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(v, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);       /* blocks the list below */
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = mk_label(v, APP_FONT_LARGE, APP_COLOR_TEXT);
    lv_label_set_text(t, "Add a symbol");
    lv_obj_set_pos(t, 16, 10);

    U.ta = lv_textarea_create(v);
    lv_textarea_set_one_line(U.ta, true);
    lv_textarea_set_max_length(U.ta, SYM_LEN - 1);
    lv_textarea_set_accepted_chars(U.ta,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-^=");
    lv_textarea_set_placeholder_text(U.ta, "e.g. AAPL");
    lv_obj_set_style_text_font(U.ta, APP_FONT_LARGE, 0);
    lv_obj_set_size(U.ta, 420, 60);
    lv_obj_set_pos(U.ta, 16, 52);
    lv_obj_add_state(U.ta, LV_STATE_FOCUSED);       /* show the cursor */

    lv_obj_t *ok = mk_button(v, LV_SYMBOL_OK "  Add", 150, 60, add_btn_event, (void *)1);
    lv_obj_set_pos(ok, 452, 52);
    lv_obj_set_style_bg_color(ok, lv_color_hex(0x1B8F4A), 0);
    lv_obj_t *cancel = mk_button(v, "Cancel", 168, 60, add_btn_event, NULL);
    lv_obj_set_pos(cancel, 616, 52);

    U.add_msg = mk_label(v, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT);
    lv_label_set_text(U.add_msg, "Yahoo Finance symbols: AAPL, VOO, ^GSPC (S&P 500), "
                                 "BTC-USD (Bitcoin), GC=F (gold), SHOP.TO (Toronto).");
    lv_label_set_long_mode(U.add_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(U.add_msg, 768);
    lv_obj_set_pos(U.add_msg, 16, 124);

    lv_obj_t *kb = lv_keyboard_create(v);
    lv_obj_set_size(kb, 800, 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
    lv_keyboard_set_textarea(kb, U.ta);
    lv_obj_add_event_cb(kb, add_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, add_kb_event, LV_EVENT_CANCEL, NULL);
}

static void refresh_clicked(lv_event_t *e)
{
    (void)e;
    refresh_all();
    update_status();
    for (int i = 0; i < D.n; i++) update_row(i);
}

static void build_list(void)
{
    lv_obj_t *v = U.list_view = mk_view();

    lv_obj_t *icon = lv_img_create(v);
    lv_img_set_src(icon, &img_stonks_small);
    lv_obj_set_pos(icon, 14, 10);

    U.status = mk_label(v, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT);
    lv_obj_set_pos(U.status, 60, 18);

    lv_obj_t *add = mk_button(v, LV_SYMBOL_PLUS "  Add", 110, 44, add_clicked, NULL);
    lv_obj_align(add, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_t *ref = mk_button(v, LV_SYMBOL_REFRESH, 56, 44, refresh_clicked, NULL);
    lv_obj_align(ref, LV_ALIGN_TOP_RIGHT, -134, 8);

    U.list = lv_obj_create(v);
    lv_obj_remove_style_all(U.list);
    lv_obj_set_size(U.list, LV_PCT(100), 416 - 60);
    lv_obj_set_pos(U.list, 0, 60);
    lv_obj_set_style_pad_hor(U.list, 12, 0);
    lv_obj_set_style_pad_bottom(U.list, 12, 0);
    lv_obj_set_style_pad_row(U.list, 8, 0);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);

    U.empty = mk_label(v, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT);
    lv_label_set_text(U.empty, "Your list is empty. Tap  " LV_SYMBOL_PLUS " Add  to watch a stock.");
    lv_obj_align(U.empty, LV_ALIGN_CENTER, 0, 20);
}

/* ======================= timer + entry points ======================= */

static void handle_result(quote_t *q)
{
    if (q->range == R_EARN) {
        int i = find_sym(q->sym);
        if (i >= 0) {
            entry_t *en = &D.ent[i];
            en->earn_pending = false;
            if (q->ok) {
                en->earn_next = q->earn_next;
                en->earn_last = q->earn_last;
            }
            en->earn_due_us = esp_timer_get_time() + (q->ok ? EARN_OK_US : EARN_FAIL_US);
            if (U.root) {
                update_row(i);
                if (U.detail_view && strcmp(D.detail_sym, en->sym) == 0) update_detail();
            }
        }
        free(q);
    } else if (q->range == R_1D) {
        int i = find_sym(q->sym);
        if (i < 0) {                         /* removed meanwhile */
            free(q);
            return;
        }
        entry_t *en = &D.ent[i];
        if (!q->ok && en->q && en->q->ok) {  /* keep showing the older data */
            en->failed = true;
            free(q);
        } else {
            free(en->q);
            en->q = q;
            en->failed = false;
            if (q->ok) D.updated = time(NULL);
        }
        if (U.root) {
            update_row(i);
            if (U.detail_view && strcmp(D.detail_sym, en->sym) == 0) update_detail();
        }
    } else if (strcmp(q->sym, D.detail_sym) == 0 && q->range == D.detail_range) {
        free(D.detail);
        D.detail = q;
        if (U.detail_view) update_detail();
    } else {
        free(q);
    }
}

/* Takes in finished downloads and starts the next refresh when due. */
static void service(void)
{
    quote_t *q;
    while (s_results && xQueueReceive(s_results, &q, 0) == pdTRUE) {
        handle_result(q);
    }

    /* Prices every 15 minutes; the refresh button is there for sooner. */
    int64_t now = esp_timer_get_time();
    if (D.last_refresh_us == 0 || now - D.last_refresh_us > REFRESH_US) {
        refresh_all();
    }

    /* Earnings dates, once the prices are in (they tell which symbols are
     * company stocks; ETFs, indexes and crypto have no earnings calls). */
    if (inflight() == 0 && net_time_wifi_connected()) {
        for (int i = 0; i < D.n; i++) {
            entry_t *en = &D.ent[i];
            if (en->q && en->q->ok && en->q->equity && !en->earn_pending &&
                now >= en->earn_due_us) {
                en->earn_pending = request(en->sym, R_EARN);
            }
        }
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    service();
    update_status();
    if (U.detail_view && D.detail_range != R_1D && !detail_quote()) {
        lv_obj_invalidate(U.d_chart);        /* "Loading..." / "No data yet" */
    }
}

/* ---------- info ticker ---------- */

void stonks_background_tick(void)
{
    if (!D.loaded) load_list();
    worker_init();
    service();
}

int stonks_ticker_items(char (*out)[TICKER_ITEM_LEN], int max)
{
    int n = 0;
    for (int i = 0; i < D.n && n < max; i++) {
        const quote_t *q = D.ent[i].q;
        if (!q || !q->ok) continue;
        char price[40];
        fmt_money(price, sizeof(price), q->price, q->currency);
        double d = q->price - q->prev_close;
        uint32_t c = change_color(q->price, q->prev_close);
        int len = snprintf(out[n], TICKER_ITEM_LEN, "#FFD54F %s#  %s  #%06" PRIX32 " %s %+.2f%%#",
                           q->sym, price, c,
                           c == C_UP ? LV_SYMBOL_UP : c == C_DOWN ? LV_SYMBOL_DOWN : "",
                           q->prev_close ? d / q->prev_close * 100.0 : 0.0);
        bool upcoming;
        int edate = earnings_date(&D.ent[i], &upcoming);
        if (edate && len > 0 && len < TICKER_ITEM_LEN) {
            char ed[16];
            fmt_ymd(ed, sizeof(ed), edate, false);
            if (upcoming && edate == today_ymd()) snprintf(ed, sizeof(ed), "today");
            snprintf(out[n] + len, TICKER_ITEM_LEN - len, "   #B0BEC5 %s# %s",
                     upcoming ? "Earnings" : "Reported", ed);
        }
        n++;
    }
    return n;
}

static void stonks_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.root = parent;
    U.scrub = -1;
    if (!D.loaded) load_list();
    worker_init();

    lv_obj_set_style_bg_color(parent, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    build_list();
    build_rows();
    update_status();
    tick(NULL);
    U.timer = lv_timer_create(tick, 250, NULL);
}

static void stonks_destroy(void)
{
    if (U.timer) lv_timer_del(U.timer);
    memset(&U, 0, sizeof(U));
}

static void stonks_tile_icon(lv_obj_t *holder, int size)
{
    lv_obj_t *img = lv_img_create(holder);
    lv_img_set_src(img, &img_stonks_icon);
    lv_obj_center(img);
    (void)size;
}

const app_t stonks_app = {
    .name      = "St0nks",
    .symbol    = LV_SYMBOL_UP,
    .color_hex = 0x2E7D32,
    .create    = stonks_create,
    .destroy   = stonks_destroy,
    .draw_icon = stonks_tile_icon,
};
