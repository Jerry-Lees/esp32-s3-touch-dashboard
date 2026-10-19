/*
 * Weather app: current conditions, next 24 hours and a 7-day forecast
 * for a selectable list of locations. Data from Open-Meteo (free, no key).
 *
 * Downloads run in a separate FreeRTOS task so the screen never freezes.
 * That task never touches LVGL; it hands its result to the UI through
 * s_fetch_state, which the LVGL timer polls.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "lvgl.h"
#include "app_framework.h"
#include "net_time.h"
#include "settings.h"

static const char *TAG = "weather";

/* ---------------------------------------------------------------------------
 * Locations (the default is picked in Settings; first entry if unset)
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    double lat;
    double lon;
} wx_loc_t;

static const wx_loc_t s_locs[] = {
    { "Lee's Summit, MO", 38.9108, -94.3822 },
    { "Columbus, OH",     39.9612, -82.9988 },
    { "Atlanta, GA",      33.7490, -84.3880 },
    { "Warsaw, MO",       38.2431, -93.3819 },
    { "Branson, MO",      36.6437, -93.2185 },
    { "Cincinnati, OH",   39.1031, -84.5120 },
};
#define LOC_COUNT   ((int)(sizeof(s_locs) / sizeof(s_locs[0])))

#define REFRESH_SEC (15 * 60)  /* re-download a location after 15 minutes */
#define RETRY_MS    15000      /* wait between failed attempts */

#define HOURS       24
#define DAYS        7
#define HOUR_SLOTS  8          /* hourly strip shows every 3rd hour */
#define HOUR_STEP   3

#define DEG "\xC2\xB0"         /* degree sign (UTF-8) */

/* ---------------------------------------------------------------------------
 * Data
 * ------------------------------------------------------------------------- */
typedef struct {
    bool    valid;
    int64_t fetched_us;        /* esp_timer time of download */
    time_t  fetched_wall;      /* wall-clock time of download */

    float temp, feels, wind;
    int   humidity, code;
    bool  is_day;

    int n_hours;
    struct { int hour; float temp; int code; int pop; bool is_day; } hourly[HOURS];

    int n_days;
    struct { int wday; float hi, lo; int code; int pop; } daily[DAYS];

    char sunrise[12], sunset[12];
} wx_data_t;

static wx_data_t s_cache[LOC_COUNT];
static bool      s_error[LOC_COUNT];
static int64_t   s_last_attempt_us[LOC_COUNT];
static int       s_sel = 0;    /* set to the Settings default on each open */

enum { FETCH_IDLE, FETCH_BUSY, FETCH_DONE };
static int       s_fetch_state = FETCH_IDLE;
static int       s_fetch_loc;
static bool      s_fetch_ok;
static wx_data_t s_fetch_result;

/* ---------------------------------------------------------------------------
 * Weather codes (WMO)
 * ------------------------------------------------------------------------- */
typedef enum { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG,
               WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM } wx_kind_t;

static wx_kind_t wx_kind(int c)
{
    if (c == 0) return WX_CLEAR;
    if (c == 1 || c == 2) return WX_PARTLY;
    if (c == 3) return WX_CLOUDY;
    if (c == 45 || c == 48) return WX_FOG;
    if (c >= 51 && c <= 57) return WX_DRIZZLE;
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return WX_RAIN;
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_SNOW;
    if (c >= 95 && c <= 99) return WX_STORM;
    return WX_CLOUDY;
}

static const char *wx_text(int c, bool day)
{
    switch (c) {
    case 0:  return day ? "Sunny" : "Clear";
    case 1:  return day ? "Mostly Sunny" : "Mostly Clear";
    case 2:  return "Partly Cloudy";
    case 3:  return "Cloudy";
    case 45: return "Fog";
    case 48: return "Freezing Fog";
    case 51: return "Light Drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy Drizzle";
    case 56: case 57: return "Freezing Drizzle";
    case 61: return "Light Rain";
    case 63: return "Rain";
    case 65: return "Heavy Rain";
    case 66: case 67: return "Freezing Rain";
    case 71: return "Light Snow";
    case 73: return "Snow";
    case 75: return "Heavy Snow";
    case 77: return "Snow Grains";
    case 80: return "Light Showers";
    case 81: return "Showers";
    case 82: return "Heavy Showers";
    case 85: case 86: return "Snow Showers";
    case 95: return "Thunderstorms";
    case 96: case 99: return "Storms + Hail";
    case 97: return "Heavy Storms";
    default: return "Cloudy";
    }
}

/* ---------------------------------------------------------------------------
 * Weather icons, drawn from simple shapes (no image files needed)
 * ------------------------------------------------------------------------- */
#define C_SUN      0xFFC107
#define C_MOON     0xE3E8EE
#define C_CLOUD_LT 0xECEFF1
#define C_CLOUD    0xB0BEC5
#define C_CLOUD_DK 0x78909C
#define C_RAIN     0x4FC3F7
#define C_SNOW     0xFFFFFF
#define C_FOG      0x90A4AE

static lv_obj_t *blob(lv_obj_t *p, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

#define P(v) ((s) * (v) / 100)

static void draw_cloud(lv_obj_t *p, int s, int dy, uint32_t col)
{
    blob(p, P(10), P(45) + dy, P(80), P(30), col);
    blob(p, P(15), P(30) + dy, P(38), P(38), col);
    blob(p, P(36), P(18) + dy, P(48), P(48), col);
}

static void draw_sun(lv_obj_t *p, int s, int x, int y, int d)
{
    lv_obj_t *sun = blob(p, x, y, d, d, C_SUN);
    lv_obj_set_style_shadow_color(sun, lv_color_hex(C_SUN), 0);
    lv_obj_set_style_shadow_width(sun, P(20), 0);
    lv_obj_set_style_shadow_opa(sun, LV_OPA_40, 0);
}

static void draw_moon(lv_obj_t *p, int s, int x, int y, int d, uint32_t bg)
{
    blob(p, x, y, d, d, C_MOON);
    blob(p, x + d * 30 / 100, y - d * 15 / 100, d * 85 / 100, d * 85 / 100, bg);
}

/* Draws the icon for a weather code into `holder` (an s x s box).
 * `bg` is the colour behind the icon (used to cut out the crescent moon). */
static void draw_wx_icon(lv_obj_t *holder, int s, int code, bool day, uint32_t bg)
{
    lv_obj_clean(holder);
    wx_kind_t k = wx_kind(code);

    switch (k) {
    case WX_CLEAR:
        if (day) draw_sun(holder, s, P(20), P(20), P(60));
        else     draw_moon(holder, s, P(20), P(20), P(60), bg);
        break;

    case WX_PARTLY:
        if (day) draw_sun(holder, s, P(6), P(6), P(46));
        else     draw_moon(holder, s, P(6), P(8), P(42), bg);
        draw_cloud(holder, s, P(8), C_CLOUD_LT);
        break;

    case WX_CLOUDY:
        draw_cloud(holder, s, 0, C_CLOUD);
        break;

    case WX_FOG:
        for (int i = 0; i < 3; i++) {
            blob(holder, P(15) + (i % 2) * P(8), P(30) + i * P(18), P(62), P(9), C_FOG);
        }
        break;

    case WX_DRIZZLE:
    case WX_RAIN: {
        draw_cloud(holder, s, -P(12), C_CLOUD_DK);
        int n = (k == WX_RAIN) ? 3 : 2;
        for (int i = 0; i < n; i++) {
            int x = (n == 3) ? P(27) + i * P(20) : P(37) + i * P(20);
            blob(holder, x, P(70), P(7), P(20), C_RAIN);
        }
        break;
    }

    case WX_SNOW:
        draw_cloud(holder, s, -P(12), C_CLOUD);
        for (int i = 0; i < 3; i++) {
            blob(holder, P(25) + i * P(20), P(72) + (i % 2) * P(8), P(11), P(11), C_SNOW);
        }
        break;

    case WX_STORM: {
        draw_cloud(holder, s, -P(14), C_CLOUD_DK);
        lv_obj_t *bolt = lv_label_create(holder);
        lv_label_set_text(bolt, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_color(bolt, lv_color_hex(C_SUN), 0);
        lv_obj_set_style_text_font(bolt, s >= 64 ? APP_FONT_LARGE : LV_FONT_DEFAULT, 0);
        lv_obj_align(bolt, LV_ALIGN_BOTTOM_MID, 0, 0);
        break;
    }
    }
}

/* Used by the launcher tile. */
static void weather_tile_icon(lv_obj_t *holder, int size)
{
    draw_wx_icon(holder, size, 2, true, 0);
}

/* ---------------------------------------------------------------------------
 * Download + parse (runs in its own task; no LVGL calls here)
 * ------------------------------------------------------------------------- */
typedef struct {
    char *buf;
    int   len;
    int   cap;
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

static double jarr_num(const cJSON *obj, const char *key, int i, double def)
{
    const cJSON *it = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj, key), i);
    return cJSON_IsNumber(it) ? it->valuedouble : def;
}

static const char *jarr_str(const cJSON *obj, const char *key, int i)
{
    const cJSON *it = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(obj, key), i);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static int jarr_size(const cJSON *obj, const char *key)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
}

static long long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* "2026-10-18T07:28" -> "7:28 AM" */
static void fmt_clock(const char *iso, char *out, size_t n)
{
    if (!iso || strlen(iso) < 16) {
        snprintf(out, n, "--");
        return;
    }
    int h = atoi(iso + 11);
    int m = atoi(iso + 14);
    snprintf(out, n, "%d:%02d %s", (h % 12) ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
}

static bool parse_weather(const char *json, wx_data_t *d)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *hr  = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    const cJSON *dy  = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cur || !hr || !dy) {
        ESP_LOGW(TAG, "Unexpected response: %.120s", json);
        cJSON_Delete(root);
        return false;
    }

    d->temp     = (float)jnum(cur, "temperature_2m", 0);
    d->feels    = (float)jnum(cur, "apparent_temperature", d->temp);
    d->humidity = (int)jnum(cur, "relative_humidity_2m", 0);
    d->wind     = (float)jnum(cur, "wind_speed_10m", 0);
    d->code     = (int)jnum(cur, "weather_code", 0);
    d->is_day   = jnum(cur, "is_day", 1) != 0;

    int n = jarr_size(hr, "time");
    if (n > HOURS) n = HOURS;
    for (int i = 0; i < n; i++) {
        const char *t = jarr_str(hr, "time", i);
        d->hourly[i].hour   = (t && strlen(t) >= 13) ? atoi(t + 11) : 0;
        d->hourly[i].temp   = (float)jarr_num(hr, "temperature_2m", i, 0);
        d->hourly[i].code   = (int)jarr_num(hr, "weather_code", i, 0);
        d->hourly[i].pop    = (int)jarr_num(hr, "precipitation_probability", i, -1);
        d->hourly[i].is_day = jarr_num(hr, "is_day", i, 1) != 0;
    }
    d->n_hours = n;

    n = jarr_size(dy, "time");
    if (n > DAYS) n = DAYS;
    for (int i = 0; i < n; i++) {
        const char *t = jarr_str(dy, "time", i);
        int y = 1970, m = 1, dd = 1;
        if (t) sscanf(t, "%d-%d-%d", &y, &m, &dd);
        d->daily[i].wday = (int)((days_from_civil(y, m, dd) % 7 + 11) % 7); /* 1970-01-01 = Thu */
        d->daily[i].hi   = (float)jarr_num(dy, "temperature_2m_max", i, 0);
        d->daily[i].lo   = (float)jarr_num(dy, "temperature_2m_min", i, 0);
        d->daily[i].code = (int)jarr_num(dy, "weather_code", i, 0);
        d->daily[i].pop  = (int)jarr_num(dy, "precipitation_probability_max", i, -1);
    }
    d->n_days = n;

    fmt_clock(jarr_str(dy, "sunrise", 0), d->sunrise, sizeof(d->sunrise));
    fmt_clock(jarr_str(dy, "sunset", 0), d->sunset, sizeof(d->sunset));

    cJSON_Delete(root);
    return d->n_hours > 0 && d->n_days > 0;
}

static void fetch_task(void *arg)
{
    int loc = (int)(intptr_t)arg;
    wx_data_t *out = &s_fetch_result;
    memset(out, 0, sizeof(*out));
    bool ok = false;

    char url[640];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
             "weather_code,wind_speed_10m,is_day"
             "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,"
             "precipitation_probability_max,sunrise,sunset"
             "&temperature_unit=fahrenheit&wind_speed_unit=mph"
             "&timezone=auto&forecast_days=7&forecast_hours=24",
             s_locs[loc].lat, s_locs[loc].lon);

    http_buf_t hb = { .cap = 24 * 1024 };
    hb.buf = heap_caps_malloc(hb.cap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hb.buf) {
        hb.buf = malloc(hb.cap + 1);
    }

    if (hb.buf) {
        hb.buf[0] = '\0';
        esp_http_client_config_t cfg = {
            .url               = url,
            .event_handler     = http_event,
            .user_data         = &hb,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms        = 15000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_err_t err = esp_http_client_perform(client);
            int status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (err == ESP_OK && status == 200 && !hb.overflow) {
                ok = parse_weather(hb.buf, out);
            } else {
                ESP_LOGW(TAG, "Download failed: %s, HTTP %d%s", esp_err_to_name(err),
                         status, hb.overflow ? ", response too large" : "");
            }
        }
        free(hb.buf);
    }

    if (ok) {
        ESP_LOGI(TAG, "%s: %.0fF, code %d", s_locs[loc].name, out->temp, out->code);
    }
    out->valid = ok;
    s_fetch_loc = loc;
    s_fetch_ok = ok;
    __atomic_store_n(&s_fetch_state, FETCH_DONE, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

static void start_fetch(int loc)
{
    if (__atomic_load_n(&s_fetch_state, __ATOMIC_ACQUIRE) != FETCH_IDLE) {
        return;
    }
    s_last_attempt_us[loc] = esp_timer_get_time();
    __atomic_store_n(&s_fetch_state, FETCH_BUSY, __ATOMIC_RELEASE);
    if (xTaskCreate(fetch_task, "wx_fetch", 10240, (void *)(intptr_t)loc, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not start download task");
        __atomic_store_n(&s_fetch_state, FETCH_IDLE, __ATOMIC_RELEASE);
    }
}

/* ---------------------------------------------------------------------------
 * UI
 * ------------------------------------------------------------------------- */
typedef struct {
    bool      active;
    lv_obj_t *dd, *updated, *status;
    lv_obj_t *cur_icon, *cur_temp, *cur_cond, *cur_details, *cur_today;
    lv_obj_t *sun_times;
    lv_obj_t *h_time[HOUR_SLOTS], *h_icon[HOUR_SLOTS], *h_temp[HOUR_SLOTS], *h_pop[HOUR_SLOTS];
    lv_obj_t *d_name[DAYS], *d_icon[DAYS], *d_hi[DAYS], *d_lo[DAYS], *d_pop[DAYS];
} wx_ui_t;

static wx_ui_t     ui;
static lv_timer_t *s_timer;

static int rnd(float f) { return (int)(f < 0 ? f - 0.5f : f + 0.5f); }

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *mk_col_label(lv_obj_t *p, const lv_font_t *font, uint32_t color,
                              int x, int y, int w)
{
    lv_obj_t *l = mk_label(p, font, color, "");
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

static lv_obj_t *mk_icon_box(lv_obj_t *p, int x, int y, int s)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, s, s);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static lv_obj_t *mk_panel(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 16, 0);
    lv_obj_set_style_pad_all(o, 12, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static const char *const WDAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* Fills every widget from the cached data for the selected location. */
static void render(void)
{
    if (!ui.active) return;
    const wx_data_t *d = &s_cache[s_sel];
    char buf[96];

    if (!d->valid) {
        lv_obj_clean(ui.cur_icon);
        lv_label_set_text(ui.cur_temp, "--" DEG);
        lv_label_set_text(ui.cur_cond, "");
        lv_label_set_text(ui.cur_details, "");
        lv_label_set_text(ui.cur_today, "");
        lv_label_set_text(ui.sun_times, "");
        for (int i = 0; i < HOUR_SLOTS; i++) {
            lv_label_set_text(ui.h_time[i], "");
            lv_label_set_text(ui.h_temp[i], "");
            lv_label_set_text(ui.h_pop[i], "");
            lv_obj_clean(ui.h_icon[i]);
        }
        for (int i = 0; i < DAYS; i++) {
            lv_label_set_text(ui.d_name[i], "");
            lv_label_set_text(ui.d_hi[i], "");
            lv_label_set_text(ui.d_lo[i], "");
            lv_label_set_text(ui.d_pop[i], "");
            lv_obj_clean(ui.d_icon[i]);
        }
        return;
    }

    /* Current conditions */
    draw_wx_icon(ui.cur_icon, 84, d->code, d->is_day, APP_COLOR_PANEL);
    snprintf(buf, sizeof(buf), "%d" DEG, rnd(d->temp));
    lv_label_set_text(ui.cur_temp, buf);
    lv_label_set_text(ui.cur_cond, wx_text(d->code, d->is_day));
    snprintf(buf, sizeof(buf), "Feels like %d" DEG "    Humidity %d%%\nWind %d mph",
             rnd(d->feels), d->humidity, rnd(d->wind));
    lv_label_set_text(ui.cur_details, buf);

    if (d->n_days > 0) {
        int pop = d->daily[0].pop;
        if (pop >= 0) {
            snprintf(buf, sizeof(buf), "H %d" DEG "   L %d" DEG "   Rain %d%%",
                     rnd(d->daily[0].hi), rnd(d->daily[0].lo), pop);
        } else {
            snprintf(buf, sizeof(buf), "H %d" DEG "   L %d" DEG,
                     rnd(d->daily[0].hi), rnd(d->daily[0].lo));
        }
        lv_label_set_text(ui.cur_today, buf);
    }

    snprintf(buf, sizeof(buf), "Sunrise %s   Sunset %s", d->sunrise, d->sunset);
    lv_label_set_text(ui.sun_times, buf);

    /* Next 24 hours, every 3rd hour */
    for (int i = 0; i < HOUR_SLOTS; i++) {
        int idx = i * HOUR_STEP;
        if (idx >= d->n_hours) {
            lv_label_set_text(ui.h_time[i], "");
            lv_label_set_text(ui.h_temp[i], "");
            lv_label_set_text(ui.h_pop[i], "");
            lv_obj_clean(ui.h_icon[i]);
            continue;
        }
        int h = d->hourly[idx].hour;
        if (i == 0) {
            lv_label_set_text(ui.h_time[i], "Now");
        } else {
            snprintf(buf, sizeof(buf), "%d %s", (h % 12) ? h % 12 : 12, h < 12 ? "AM" : "PM");
            lv_label_set_text(ui.h_time[i], buf);
        }
        draw_wx_icon(ui.h_icon[i], 40, d->hourly[idx].code, d->hourly[idx].is_day,
                     APP_COLOR_PANEL);
        snprintf(buf, sizeof(buf), "%d" DEG, rnd(d->hourly[idx].temp));
        lv_label_set_text(ui.h_temp[i], buf);
        if (d->hourly[idx].pop > 0) {
            snprintf(buf, sizeof(buf), "%d%%", d->hourly[idx].pop);
            lv_label_set_text(ui.h_pop[i], buf);
        } else {
            lv_label_set_text(ui.h_pop[i], "");
        }
    }

    /* 7-day forecast */
    for (int i = 0; i < DAYS; i++) {
        if (i >= d->n_days) {
            lv_label_set_text(ui.d_name[i], "");
            lv_label_set_text(ui.d_hi[i], "");
            lv_label_set_text(ui.d_lo[i], "");
            lv_label_set_text(ui.d_pop[i], "");
            lv_obj_clean(ui.d_icon[i]);
            continue;
        }
        lv_label_set_text(ui.d_name[i], i == 0 ? "Today" : WDAY[d->daily[i].wday]);
        draw_wx_icon(ui.d_icon[i], 40, d->daily[i].code, true, APP_COLOR_PANEL);
        snprintf(buf, sizeof(buf), "%d" DEG, rnd(d->daily[i].hi));
        lv_label_set_text(ui.d_hi[i], buf);
        snprintf(buf, sizeof(buf), "%d" DEG, rnd(d->daily[i].lo));
        lv_label_set_text(ui.d_lo[i], buf);
        if (d->daily[i].pop > 0) {
            snprintf(buf, sizeof(buf), "Rain %d%%", d->daily[i].pop);
            lv_label_set_text(ui.d_pop[i], buf);
        } else {
            lv_label_set_text(ui.d_pop[i], "");
        }
    }
}

/* Updates the small "Updated ..." text and the centre status message. */
static void update_status(void)
{
    if (!ui.active) return;
    const wx_data_t *d = &s_cache[s_sel];
    bool busy = __atomic_load_n(&s_fetch_state, __ATOMIC_ACQUIRE) == FETCH_BUSY;

    if (d->valid) {
        lv_obj_add_flag(ui.status, LV_OBJ_FLAG_HIDDEN);
        if (busy) {
            lv_label_set_text(ui.updated, "Updating...");
        } else if (s_error[s_sel]) {
            lv_label_set_text(ui.updated, "Update failed, retrying");
        } else if (d->fetched_wall) {
            struct tm tm;
            localtime_r(&d->fetched_wall, &tm);
            char buf[32], t[16];
            strftime(t, sizeof(t), "%I:%M %p", &tm);
            snprintf(buf, sizeof(buf), "Updated %s", t[0] == '0' ? t + 1 : t);
            lv_label_set_text(ui.updated, buf);
        } else {
            lv_label_set_text(ui.updated, "");
        }
        return;
    }

    const char *msg;
    if (!net_time_wifi_configured())       msg = "Wi-Fi is not set up (see wifi_secrets.h)";
    else if (!net_time_wifi_connected())   msg = "Waiting for Wi-Fi...";
    else if (!net_time_is_synced())        msg = "Waiting for the clock to sync...";
    else if (s_error[s_sel] && !busy)      msg = "Couldn't load the weather. Retrying...";
    else                                   msg = "Loading weather...";
    lv_label_set_text(ui.status, msg);
    lv_obj_clear_flag(ui.status, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ui.updated, "");
}

/* Starts a download of `loc` if its data is old (or `force`). Returns true
 * if one was started. Only one download runs at a time. */
static bool maybe_fetch_loc(int loc, bool force)
{
    if (!net_time_wifi_connected() || !net_time_is_synced()) {
        return false;
    }
    const wx_data_t *d = &s_cache[loc];
    int64_t now = esp_timer_get_time();
    bool stale = !d->valid || (now - d->fetched_us) > (int64_t)REFRESH_SEC * 1000000;
    bool may_retry = s_last_attempt_us[loc] == 0 ||
                     (now - s_last_attempt_us[loc]) > (int64_t)RETRY_MS * 1000;
    if ((force || (stale && may_retry)) &&
        __atomic_load_n(&s_fetch_state, __ATOMIC_ACQUIRE) == FETCH_IDLE) {
        start_fetch(loc);
        return true;
    }
    return false;
}

static void maybe_fetch(bool force)
{
    maybe_fetch_loc(s_sel, force);
}

/* Takes a finished download into the cache (LVGL task: app timer or the
 * ticker's background service). */
static void collect_fetch(void)
{
    if (__atomic_load_n(&s_fetch_state, __ATOMIC_ACQUIRE) != FETCH_DONE) {
        return;
    }
    int loc = s_fetch_loc;
    if (s_fetch_ok) {
        s_cache[loc] = s_fetch_result;
        s_cache[loc].fetched_us = esp_timer_get_time();
        s_cache[loc].fetched_wall = net_time_is_synced() ? time(NULL) : 0;
        s_error[loc] = false;
    } else {
        s_error[loc] = true;
    }
    __atomic_store_n(&s_fetch_state, FETCH_IDLE, __ATOMIC_RELEASE);
    if (loc == s_sel) {
        render();
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    collect_fetch();
    maybe_fetch(false);
    update_status();
}

/* ---------------------------------------------------------------------------
 * Info ticker (home screen): background refresh + text
 * ------------------------------------------------------------------------- */
void weather_background_tick(void)
{
    collect_fetch();
    uint32_t mask = settings_get_ticker_cities();
    for (int i = 0; i < LOC_COUNT; i++) {
        if ((mask & (1u << i)) && maybe_fetch_loc(i, false)) {
            break;
        }
    }
}

int weather_ticker_items(char (*out)[TICKER_ITEM_LEN], int max)
{
    uint32_t mask = settings_get_ticker_cities();
    int n = 0;
    for (int i = 0; i < LOC_COUNT && n < max; i++) {
        if (!(mask & (1u << i))) continue;
        const wx_data_t *d = &s_cache[i];
        char *o = out[n++];
        size_t len = TICKER_ITEM_LEN;
        if (!d->valid) {
            snprintf(o, len, "#4FC3F7 %s#  loading...", s_locs[i].name);
            continue;
        }
        int k = snprintf(o, len, "#4FC3F7 %s#  #FFFFFF %d" DEG "F# %s",
                         s_locs[i].name, rnd(d->temp), wx_text(d->code, d->is_day));
        /* Today and tomorrow only; the app has the rest of the week. */
        for (int j = 0; j < d->n_days && j < 2 && k > 0 && k < (int)len; j++) {
            k += snprintf(o + k, len - k, "   #B0BEC5 %s# %d" DEG "/%d" DEG " %s",
                          j == 0 ? "Today" : "Tomorrow",
                          rnd(d->daily[j].hi), rnd(d->daily[j].lo),
                          wx_text(d->daily[j].code, true));
            if (k < (int)len && d->daily[j].pop >= 30) {
                k += snprintf(o + k, len - k, " (rain %d%%)", d->daily[j].pop);
            }
        }
    }
    return n;
}

static void location_changed(lv_event_t *e)
{
    s_sel = (int)lv_dropdown_get_selected(lv_event_get_target(e));
    render();
    maybe_fetch(false);
    update_status();
}

static void refresh_clicked(lv_event_t *e)
{
    (void)e;
    maybe_fetch(true);
    update_status();
}

int weather_location_count(void) { return LOC_COUNT; }

const char *weather_location_name(int index)
{
    return (index >= 0 && index < LOC_COUNT) ? s_locs[index].name : "";
}

static void weather_create(lv_obj_t *parent)
{
    memset(&ui, 0, sizeof(ui));

    s_sel = settings_get_weather_city();
    if (s_sel < 0 || s_sel >= LOC_COUNT) {
        s_sel = 0;
    }

    /* ---- Top row: location picker, last update, refresh ---- */
    char opts[256] = "";
    for (int i = 0; i < LOC_COUNT; i++) {
        if (i) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
        strncat(opts, s_locs[i].name, sizeof(opts) - strlen(opts) - 1);
    }
    ui.dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(ui.dd, opts);
    lv_dropdown_set_selected(ui.dd, s_sel);
    lv_obj_set_pos(ui.dd, 14, 8);
    lv_obj_set_width(ui.dd, 300);
    lv_obj_set_style_text_font(ui.dd, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_bg_color(ui.dd, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_text_color(ui.dd, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_set_style_border_color(ui.dd, lv_color_hex(0x3A4652), 0);
    lv_obj_t *list = lv_dropdown_get_list(ui.dd);
    if (list) {
        lv_obj_set_style_text_font(list, APP_FONT_MEDIUM, 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(APP_COLOR_PANEL), 0);
        lv_obj_set_style_text_color(list, lv_color_hex(APP_COLOR_TEXT), 0);
        lv_obj_set_style_max_height(list, 340, 0);
    }
    lv_obj_add_event_cb(ui.dd, location_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *refresh = lv_btn_create(parent);
    lv_obj_set_size(refresh, 50, 44);
    lv_obj_set_pos(refresh, 736, 8);
    lv_obj_set_style_radius(refresh, 22, 0);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = mk_label(refresh, APP_FONT_MEDIUM, APP_COLOR_TEXT, LV_SYMBOL_REFRESH);
    lv_obj_center(rl);

    ui.updated = mk_label(parent, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_set_width(ui.updated, 380);
    lv_obj_set_style_text_align(ui.updated, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(ui.updated, 340, 22);

    /* ---- Current conditions ---- */
    lv_obj_t *cur = mk_panel(parent, 14, 64, 300, 198);
    ui.cur_icon = mk_icon_box(cur, 0, 0, 84);
    ui.cur_temp = mk_label(cur, APP_FONT_HUGE, APP_COLOR_TEXT, "--" DEG);
    lv_obj_set_pos(ui.cur_temp, 100, 2);
    ui.cur_cond = mk_label(cur, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_obj_set_width(ui.cur_cond, 176);
    lv_label_set_long_mode(ui.cur_cond, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(ui.cur_cond, 100, 58);
    ui.cur_details = mk_label(cur, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_set_pos(ui.cur_details, 0, 96);
    ui.cur_today = mk_label(cur, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_obj_align(ui.cur_today, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ---- Next 24 hours ---- */
    lv_obj_t *hrs = mk_panel(parent, 324, 64, 462, 198);
    mk_label(hrs, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "Next 24 hours");
    ui.sun_times = mk_label(hrs, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_align(ui.sun_times, LV_ALIGN_TOP_RIGHT, 0, 0);
    const int hcol = 438 / HOUR_SLOTS;
    for (int i = 0; i < HOUR_SLOTS; i++) {
        int x = i * hcol;
        ui.h_time[i] = mk_col_label(hrs, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, x, 30, hcol);
        ui.h_icon[i] = mk_icon_box(hrs, x + (hcol - 40) / 2, 54, 40);
        ui.h_temp[i] = mk_col_label(hrs, APP_FONT_MEDIUM, APP_COLOR_TEXT, x, 102, hcol);
        ui.h_pop[i]  = mk_col_label(hrs, LV_FONT_DEFAULT, C_RAIN, x, 134, hcol);
    }

    /* ---- 7-day forecast ---- */
    lv_obj_t *days = mk_panel(parent, 14, 272, 772, 136);
    lv_obj_set_style_pad_all(days, 10, 0);
    const int dcol = 752 / DAYS;
    for (int i = 0; i < DAYS; i++) {
        int x = i * dcol;
        ui.d_name[i] = mk_col_label(days, APP_FONT_MEDIUM, APP_COLOR_TEXT, x, 0, dcol);
        ui.d_icon[i] = mk_icon_box(days, x + (dcol - 40) / 2, 26, 40);
        ui.d_hi[i] = mk_label(days, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
        lv_obj_set_width(ui.d_hi[i], dcol / 2 - 3);
        lv_obj_set_style_text_align(ui.d_hi[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(ui.d_hi[i], x, 70);
        ui.d_lo[i] = mk_label(days, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");
        lv_obj_set_width(ui.d_lo[i], dcol / 2 - 3);
        lv_obj_set_pos(ui.d_lo[i], x + dcol / 2 + 3, 70);
        ui.d_pop[i] = mk_col_label(days, LV_FONT_DEFAULT, C_RAIN, x, 96, dcol);
    }

    /* ---- Centre message shown until data arrives ---- */
    ui.status = mk_label(parent, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_obj_set_style_bg_color(ui.status, lv_color_hex(0x2A333C), 0);
    lv_obj_set_style_bg_opa(ui.status, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.status, 12, 0);
    lv_obj_set_style_pad_hor(ui.status, 20, 0);
    lv_obj_set_style_pad_ver(ui.status, 12, 0);
    lv_obj_align(ui.status, LV_ALIGN_CENTER, 0, 10);

    ui.active = true;
    render();
    maybe_fetch(false);
    update_status();
    s_timer = lv_timer_create(tick, 500, NULL);
}

static void weather_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    ui.active = false;
    /* A download still running finishes in the background and is picked up
     * the next time the app opens. */
}

const app_t weather_app = {
    .name      = "Weather",
    .symbol    = LV_SYMBOL_IMAGE,
    .color_hex = 0x00838F,
    .create    = weather_create,
    .destroy   = weather_destroy,
    .draw_icon = weather_tile_icon,
};
