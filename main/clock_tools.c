/*
 * Timers, stopwatch and alarms (see clock_tools.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "lvgl.h"
#include "app_framework.h"
#include "clock_tools.h"
#include "launcher.h"
#include "net_time.h"
#include "settings.h"

#define SNOOZE_MIN   9
#define ALERT_QUEUE  4

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void clock_tools_fmt(char *out, size_t len, int64_t ms, bool cs)
{
    if (ms < 0) ms = 0;
    int64_t s = ms / 1000;
    int h = (int)(s / 3600), m = (int)(s / 60 % 60), sec = (int)(s % 60);
    if (cs) {
        int c = (int)(ms / 10 % 100);
        if (h) snprintf(out, len, "%d:%02d:%02d.%02d", h, m, sec, c);
        else snprintf(out, len, "%d:%02d.%02d", m, sec, c);
    } else {
        if (h) snprintf(out, len, "%d:%02d:%02d", h, m, sec);
        else snprintf(out, len, "%d:%02d", m, sec);
    }
}

/* ======================= timers ======================= */

static struct {
    bool    used, running;
    int64_t end_ms;                /* running: when it ends (system time) */
    int64_t left_ms;               /* paused: time left */
    int64_t total_ms;
} T[CT_TIMERS];

int ct_timer_start(int64_t ms)
{
    if (ms <= 0) return -1;
    for (int i = 0; i < CT_TIMERS; i++) {
        if (!T[i].used) {
            T[i].used = T[i].running = true;
            T[i].total_ms = T[i].left_ms = ms;
            T[i].end_ms = now_ms() + ms;
            return i;
        }
    }
    return -1;
}

void ct_timer_pause(int i, bool pause)
{
    if (i < 0 || i >= CT_TIMERS || !T[i].used || T[i].running != pause) return;
    if (pause) {
        T[i].left_ms = T[i].end_ms - now_ms();
        if (T[i].left_ms < 0) T[i].left_ms = 0;
    } else {
        T[i].end_ms = now_ms() + T[i].left_ms;
    }
    T[i].running = !pause;
}

void ct_timer_restart(int i)
{
    if (i < 0 || i >= CT_TIMERS || !T[i].used) return;
    T[i].running = true;
    T[i].left_ms = T[i].total_ms;
    T[i].end_ms = now_ms() + T[i].total_ms;
}

void ct_timer_remove(int i)
{
    if (i >= 0 && i < CT_TIMERS) memset(&T[i], 0, sizeof(T[i]));
}

ct_timer_info_t ct_timer_info(int i)
{
    ct_timer_info_t r = {0};
    if (i < 0 || i >= CT_TIMERS || !T[i].used) return r;
    r.used = true;
    r.running = T[i].running;
    r.total_ms = T[i].total_ms;
    r.left_ms = T[i].running ? T[i].end_ms - now_ms() : T[i].left_ms;
    if (r.left_ms < 0) r.left_ms = 0;
    return r;
}

/* ======================= stopwatch ======================= */

static struct {
    bool    running;
    int64_t start_ms;              /* running: system time when (re)started */
    int64_t acc_ms;                /* time from earlier runs */
    int64_t laps[CT_LAPS];
    int     nlaps;
} SW;

void ct_sw_start(void)
{
    if (SW.running) return;
    SW.running = true;
    SW.start_ms = now_ms();
}

void ct_sw_stop(void)
{
    if (!SW.running) return;
    SW.acc_ms += now_ms() - SW.start_ms;
    SW.running = false;
}

void ct_sw_reset(void) { memset(&SW, 0, sizeof(SW)); }
bool ct_sw_running(void) { return SW.running; }

int64_t ct_sw_elapsed_ms(void)
{
    return SW.acc_ms + (SW.running ? now_ms() - SW.start_ms : 0);
}

bool ct_sw_lap(void)
{
    if (SW.nlaps >= CT_LAPS) return false;
    SW.laps[SW.nlaps++] = ct_sw_elapsed_ms();
    return true;
}

int ct_sw_laps(const int64_t **laps)
{
    *laps = SW.laps;
    return SW.nlaps;
}

/* ======================= alarms ======================= */

static ct_alarm_t A[CT_ALARMS];
static int        s_nalarms;
static int        s_last_fired[CT_ALARMS];   /* minute-of-year it last went off */
static time_t     s_snooze_at;               /* 0 = no snooze */
static int        s_snooze_hour, s_snooze_min;

/* Saved as "HHMMddo;" per alarm: hour, minute, days (hex), on (0/1). */
static void alarms_save(void)
{
    char buf[CT_ALARMS * 9 + 1];
    size_t k = 0;
    for (int i = 0; i < s_nalarms; i++) {
        k += snprintf(buf + k, sizeof(buf) - k, "%02d%02d%02x%d;", A[i].hour, A[i].min,
                      A[i].days & 0x7F, A[i].on);
    }
    buf[k] = 0;
    settings_set_str("alarms", buf);
}

static void alarms_load(void)
{
    char buf[CT_ALARMS * 9 + 1];
    s_nalarms = 0;
    if (!settings_get_str("alarms", buf, sizeof(buf))) return;
    for (const char *p = buf; *p && s_nalarms < CT_ALARMS;) {
        unsigned h, m, d, on;
        int n = 0;
        if (sscanf(p, "%2u%2u%2x%1u;%n", &h, &m, &d, &on, &n) == 4 && n && h < 24 && m < 60) {
            A[s_nalarms++] = (ct_alarm_t){ .hour = h, .min = m, .days = d & 0x7F, .on = on != 0 };
        }
        const char *semi = strchr(p, ';');
        if (!semi) break;
        p = semi + 1;
    }
}

int ct_alarm_count(void) { return s_nalarms; }

ct_alarm_t ct_alarm_get(int i)
{
    ct_alarm_t none = {0};
    return i >= 0 && i < s_nalarms ? A[i] : none;
}

void ct_alarm_set(int i, ct_alarm_t a)
{
    if (i < 0 || i >= s_nalarms) return;
    A[i] = a;
    s_last_fired[i] = -1;
    alarms_save();
}

int ct_alarm_add(ct_alarm_t a)
{
    if (s_nalarms >= CT_ALARMS) return -1;
    s_last_fired[s_nalarms] = -1;
    A[s_nalarms++] = a;
    alarms_save();
    return s_nalarms - 1;
}

void ct_alarm_remove(int i)
{
    if (i < 0 || i >= s_nalarms) return;
    for (int k = i; k < s_nalarms - 1; k++) {
        A[k] = A[k + 1];
        s_last_fired[k] = s_last_fired[k + 1];
    }
    s_nalarms--;
    alarms_save();
}

void ct_alarm_days_text(uint8_t days, char *out, size_t len)
{
    static const char *const names[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    days &= 0x7F;
    if (days == 0) { strlcpy(out, "Once", len); return; }
    if (days == 0x7F) { strlcpy(out, "Every day", len); return; }
    if (days == 0x3E) { strlcpy(out, "Weekdays", len); return; }
    if (days == 0x41) { strlcpy(out, "Weekends", len); return; }
    size_t k = 0;
    out[0] = 0;
    for (int d = 0; d < 7; d++) {
        if (days & (1 << d)) k += snprintf(out + k, len > k ? len - k : 0, "%s%s", k ? " " : "", names[d]);
    }
}

void ct_alarm_time_text(int hour, int min, char *out, size_t len)
{
    if (settings_get_24h()) snprintf(out, len, "%02d:%02d", hour, min);
    else snprintf(out, len, "%d:%02d %s", hour % 12 ? hour % 12 : 12, min, hour < 12 ? "AM" : "PM");
}

/* Next time alarm a goes off after `now` (0 if it's off). */
static time_t alarm_next(const ct_alarm_t *a, time_t now)
{
    if (!a->on) return 0;
    struct tm tm;
    localtime_r(&now, &tm);
    for (int d = 0; d <= 7; d++) {
        struct tm c = tm;
        c.tm_mday += d;
        c.tm_hour = a->hour;
        c.tm_min = a->min;
        c.tm_sec = 0;
        c.tm_isdst = -1;
        time_t t = mktime(&c);                  /* normalizes the date and weekday */
        if (t <= now) continue;
        if (a->days == 0 || (a->days & (1 << c.tm_wday))) return t;
    }
    return 0;
}

/* ======================= alerts ======================= */

typedef struct {
    char title[48];
    bool alarm;                    /* offers Snooze */
    int  hour, min;                /* alarm time (for the snooze) */
} alert_t;

static alert_t     s_queue[ALERT_QUEUE];
static int         s_nqueue;
static lv_obj_t   *s_alert;        /* the alert on screen, or NULL */
static alert_t     s_cur;
static uint32_t    s_alert_since;

static void alert_close(void)
{
    if (s_alert) lv_obj_del_async(s_alert);    /* called from its own buttons */
    s_alert = NULL;
}

static void alert_dismiss(lv_event_t *e)
{
    (void)e;
    alert_close();
}

static void alert_snooze(lv_event_t *e)
{
    (void)e;
    s_snooze_at = time(NULL) + SNOOZE_MIN * 60;
    s_snooze_hour = s_cur.hour;
    s_snooze_min = s_cur.min;
    alert_close();
    char msg[48];
    snprintf(msg, sizeof(msg), "Snoozed for %d minutes", SNOOZE_MIN);
    launcher_toast(msg, 3000);
}

static lv_obj_t *alert_btn(lv_obj_t *parent, const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 260, 90);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, APP_FONT_LARGE, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static void alert_show(const alert_t *a)
{
    launcher_wake();
    s_cur = *a;
    s_alert_since = lv_tick_get();
    s_alert = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_alert);
    lv_obj_set_size(s_alert, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_alert, lv_color_hex(0xB71C1C), 0);
    lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);         /* nothing underneath gets taps */
    lv_obj_clear_flag(s_alert, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(s_alert);
    lv_label_set_text(icon, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(icon, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *t = lv_label_create(s_alert);
    lv_label_set_text(t, a->title);
    lv_obj_set_style_text_font(t, APP_FONT_HUGE, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 140);

    if (a->alarm) {
        lv_obj_align(alert_btn(s_alert, "Snooze 9 min", 0x37474F, alert_snooze),
                     LV_ALIGN_BOTTOM_MID, -150, -60);
        lv_obj_align(alert_btn(s_alert, LV_SYMBOL_OK "  Dismiss", 0x1B5E20, alert_dismiss),
                     LV_ALIGN_BOTTOM_MID, 150, -60);
    } else {
        lv_obj_align(alert_btn(s_alert, LV_SYMBOL_OK "  Dismiss", 0x1B5E20, alert_dismiss),
                     LV_ALIGN_BOTTOM_MID, 0, -60);
    }
}

static void alert_post(const char *title, bool alarm, int hour, int min)
{
    if (s_nqueue >= ALERT_QUEUE) return;
    alert_t *a = &s_queue[s_nqueue++];
    strlcpy(a->title, title, sizeof(a->title));
    a->alarm = alarm;
    a->hour = hour;
    a->min = min;
}

bool clock_tools_alerting(void) { return s_alert != NULL || s_nqueue > 0; }

/* ======================= background check ======================= */

static void fire_alarm(int hour, int min)
{
    char t[24], title[48];
    ct_alarm_time_text(hour, min, t, sizeof(t));
    snprintf(title, sizeof(title), "Alarm  %s", t);
    alert_post(title, true, hour, min);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    /* Timers */
    for (int i = 0; i < CT_TIMERS; i++) {
        if (T[i].used && T[i].running && now_ms() >= T[i].end_ms) {
            char d[24], title[48];
            clock_tools_fmt(d, sizeof(d), T[i].total_ms, false);
            snprintf(title, sizeof(title), "Timer  %s  done", d);
            ct_timer_remove(i);
            alert_post(title, false, 0, 0);
        }
    }

    /* Alarms: only once the clock is real. */
    if (net_time_is_synced()) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        int key = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
        for (int i = 0; i < s_nalarms; i++) {
            ct_alarm_t *a = &A[i];
            if (!a->on || a->hour != tm.tm_hour || a->min != tm.tm_min || s_last_fired[i] == key) continue;
            if (a->days && !(a->days & (1 << tm.tm_wday))) continue;
            s_last_fired[i] = key;
            if (a->days == 0) {                     /* a one-time alarm turns itself off */
                a->on = false;
                alarms_save();
            }
            fire_alarm(a->hour, a->min);
        }
        if (s_snooze_at && now >= s_snooze_at) {
            s_snooze_at = 0;
            fire_alarm(s_snooze_hour, s_snooze_min);
        }
    }

    /* Alerts: one at a time; the one on screen flashes. */
    if (!s_alert && s_nqueue > 0) {
        alert_t a = s_queue[0];
        memmove(s_queue, s_queue + 1, (size_t)(s_nqueue - 1) * sizeof(alert_t));
        s_nqueue--;
        alert_show(&a);
    }
    if (s_alert) {
        bool bright = (lv_tick_elaps(s_alert_since) / 500) % 2 == 0;
        lv_obj_set_style_bg_color(s_alert, lv_color_hex(bright ? 0xC62828 : 0x3E0A0A), 0);
        lv_disp_trig_activity(NULL);               /* keep the screen awake */
    }
}

bool clock_tools_status(char *out, size_t len)
{
    int64_t best = -1;
    for (int i = 0; i < CT_TIMERS; i++) {
        ct_timer_info_t t = ct_timer_info(i);
        if (t.used && t.running && (best < 0 || t.left_ms < best)) best = t.left_ms;
    }
    if (best >= 0) {
        char d[24];
        clock_tools_fmt(d, sizeof(d), best + 999, false);   /* round up, like the app */
        snprintf(out, len, LV_SYMBOL_LOOP " %s", d);
        return true;
    }
    if (!net_time_is_synced()) return false;
    time_t now = time(NULL), next = 0;
    for (int i = 0; i < s_nalarms; i++) {
        time_t t = alarm_next(&A[i], now);
        if (t && (!next || t < next)) next = t;
    }
    if (s_snooze_at && (!next || s_snooze_at < next)) next = s_snooze_at;
    if (!next) return false;
    struct tm tm;
    localtime_r(&next, &tm);
    char t[24];
    ct_alarm_time_text(tm.tm_hour, tm.tm_min, t, sizeof(t));
    snprintf(out, len, LV_SYMBOL_BELL " %s", t);
    return true;
}

void clock_tools_start(void)
{
    alarms_load();
    for (int i = 0; i < CT_ALARMS; i++) s_last_fired[i] = -1;
    lv_timer_create(tick, 250, NULL);
}
