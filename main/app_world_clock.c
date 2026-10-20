/*
 * World Clock app: US Central (local), US Eastern, London and UTC.
 * Daylight saving time is applied automatically from the TZ rules.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "app_framework.h"
#include "net_time.h"
#include "settings.h"

/* 12/24-hour comes from Settings. UTC always uses 24-hour. */

typedef struct {
    const char *name;
    const char *place;
    const char *tz;
    bool        is_local;
    bool        force_24h;
} zone_t;

static const zone_t s_zones[] = {
    { "Central", "Chicago",                   TZ_US_CENTRAL, true,  false },
    { "Eastern", "New York",                  TZ_US_EASTERN, false, false },
    { "London",  "United Kingdom",            TZ_LONDON,     false, false },
    { "UTC",     "Coordinated Universal Time", TZ_UTC,       false, true  },
};
#define ZONE_COUNT (sizeof(s_zones) / sizeof(s_zones[0]))

typedef struct {
    lv_obj_t *time;
    lv_obj_t *ampm;
    lv_obj_t *date;
    lv_obj_t *offset;
} zone_ui_t;

static zone_ui_t   s_ui[ZONE_COUNT];
static lv_timer_t *s_timer;
static time_t      s_last_shown;

static const char *const WDAY[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MON[]  = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* Days since 1970-01-01 for a calendar date (proleptic Gregorian). */
static long long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* UTC offset in minutes of a broken-down local time. */
static int utc_offset_minutes(const struct tm *tm, time_t now)
{
    long long as_utc =
        days_from_civil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday) * 86400LL +
        tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
    return (int)((as_utc - (long long)now) / 60);
}

static void show_waiting(const char *msg)
{
    for (size_t i = 0; i < ZONE_COUNT; i++) {
        lv_label_set_text(s_ui[i].time, "--:--:--");
        lv_label_set_text(s_ui[i].ampm, "");
        lv_label_set_text(s_ui[i].date, msg);
        lv_label_set_text(s_ui[i].offset, "");
    }
}

static void update_clocks(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    if (now == s_last_shown) {
        return;
    }
    s_last_shown = now;

    if (!net_time_is_synced()) {
        if (!net_time_wifi_configured()) {
            show_waiting("No Wi-Fi: see Settings > Network");
        } else if (!net_time_wifi_connected()) {
            show_waiting("Connecting to Wi-Fi...");
        } else {
            show_waiting("Getting time from internet...");
        }
        return;
    }

    for (size_t i = 0; i < ZONE_COUNT; i++) {
        const zone_t *z = &s_zones[i];
        zone_ui_t *ui = &s_ui[i];

        setenv("TZ", z->tz, 1);
        tzset();
        struct tm tm;
        localtime_r(&now, &tm);

        char buf[48];
        bool use_24h = settings_get_24h() || z->force_24h;

        if (use_24h) {
            strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            lv_label_set_text(ui->time, buf);
            lv_label_set_text(ui->ampm, "");
        } else {
            strftime(buf, sizeof(buf), "%I:%M:%S", &tm);
            lv_label_set_text(ui->time, buf[0] == '0' ? buf + 1 : buf);
            lv_label_set_text(ui->ampm, tm.tm_hour < 12 ? "AM" : "PM");
        }
        lv_obj_align_to(ui->ampm, ui->time, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -8);

        snprintf(buf, sizeof(buf), "%s, %s %d", WDAY[tm.tm_wday],
                 MON[tm.tm_mon], tm.tm_mday);
        lv_label_set_text(ui->date, buf);

        char abbr[12];
        strftime(abbr, sizeof(abbr), "%Z", &tm);
        int off = utc_offset_minutes(&tm, now);
        if (strcmp(abbr, "UTC") == 0) {
            snprintf(buf, sizeof(buf), "UTC");
        } else if (off % 60 == 0) {
            snprintf(buf, sizeof(buf), "%s   UTC%+d", abbr, off / 60);
        } else {
            snprintf(buf, sizeof(buf), "%s   UTC%+d:%02d", abbr, off / 60,
                     abs(off % 60));
        }
        lv_label_set_text(ui->offset, buf);
    }

    /* Put the device's own zone back. */
    setenv("TZ", LOCAL_TZ, 1);
    tzset();
}

static lv_obj_t *make_card(lv_obj_t *parent, const zone_t *z, zone_ui_t *ui,
                           lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_style_border_width(card, z->is_local ? 2 : 0, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x42A5F5), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, z->name);
    lv_obj_set_style_text_font(name, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *place = lv_label_create(card);
    lv_label_set_text(place, z->place);
    lv_obj_set_style_text_color(place, lv_color_hex(APP_COLOR_SUBTEXT), 0);
    lv_obj_align_to(place, name, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    if (z->is_local) {
        lv_obj_t *badge = lv_label_create(card);
        lv_label_set_text(badge, "LOCAL");
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x42A5F5), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(badge, lv_color_hex(0x0D1B2A), 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_set_style_pad_hor(badge, 10, 0);
        lv_obj_set_style_pad_ver(badge, 4, 0);
        lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 0, 0);
    }

    ui->time = lv_label_create(card);
    lv_obj_set_style_text_font(ui->time, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(ui->time, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_label_set_text(ui->time, "--:--:--");
    lv_obj_align(ui->time, LV_ALIGN_LEFT_MID, 0, 14);

    ui->ampm = lv_label_create(card);
    lv_obj_set_style_text_font(ui->ampm, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(ui->ampm, lv_color_hex(APP_COLOR_SUBTEXT), 0);
    lv_label_set_text(ui->ampm, "");

    ui->date = lv_label_create(card);
    lv_obj_set_style_text_font(ui->date, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(ui->date, lv_color_hex(APP_COLOR_SUBTEXT), 0);
    lv_label_set_text(ui->date, "");
    lv_obj_align(ui->date, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    ui->offset = lv_label_create(card);
    lv_obj_set_style_text_color(ui->offset, lv_color_hex(APP_COLOR_SUBTEXT), 0);
    lv_label_set_text(ui->offset, "");
    lv_obj_align(ui->offset, LV_ALIGN_BOTTOM_RIGHT, 0, -2);

    return card;
}

static void world_clock_create(lv_obj_t *parent)
{
    const lv_coord_t gap = 14;

    lv_obj_update_layout(parent);
    lv_coord_t pw = lv_obj_get_width(parent);
    lv_coord_t ph = lv_obj_get_height(parent);
    lv_coord_t cw = (pw - 3 * gap) / 2;
    lv_coord_t ch = (ph - 3 * gap) / 2;

    for (size_t i = 0; i < ZONE_COUNT; i++) {
        lv_obj_t *card = make_card(parent, &s_zones[i], &s_ui[i], cw, ch);
        lv_obj_set_pos(card, gap + (i % 2) * (cw + gap), gap + (i / 2) * (ch + gap));
    }

    s_last_shown = 0;
    update_clocks(NULL);
    s_timer = lv_timer_create(update_clocks, 200, NULL);
}

static void world_clock_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    memset(s_ui, 0, sizeof(s_ui));
    setenv("TZ", LOCAL_TZ, 1);
    tzset();
}

const app_t world_clock_app = {
    .name      = "World Clock",
    .symbol    = LV_SYMBOL_GPS,
    .color_hex = 0x1565C0,
    .create    = world_clock_create,
    .destroy   = world_clock_destroy,
};
