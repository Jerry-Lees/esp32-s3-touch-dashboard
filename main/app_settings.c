/*
 * Settings app: tabs for Network, Display, Time, Weather and About.
 *
 * Choices are saved through settings.c (NVS), so they survive a reboot.
 * Other apps read them with the settings_get_*() functions.
 * Everything here runs in the LVGL task; the Wi-Fi/system calls below only
 * read state, so they are safe to call from here.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "lvgl.h"
#include "app_framework.h"
#include "launcher.h"
#include "net_time.h"
#include "settings.h"

#define C_ACCENT   0x42A5F5
#define C_BUTTON   0x2A333C
#define TAB_H      56

/* Screen timeout choices, matching the button labels below. */
static const int s_timeout_values[] = { 0, 1, 5, 15 };
static const char *s_timeout_map[] = { "Never", "1 min", "5 min", "15 min", "" };

static const char *s_clock_map[] = { "12-hour", "24-hour", "" };

/* Weather city buttons, two per row. Filled in from the Weather app's list.
 * The button matrix keeps a pointer to this, so it must stay static. */
#define MAX_CITIES 12
static const char *s_city_map[MAX_CITIES + MAX_CITIES / 2 + 1];

static lv_timer_t *s_timer;

static struct {
    lv_obj_t *net_status, *net_ssid, *net_signal, *net_ip;
    lv_obj_t *time_preview, *time_source;
    lv_obj_t *about_heap, *about_psram, *about_uptime;
} ui;

/* ---------- small UI helpers ---------- */

static lv_obj_t *mk_label(lv_obj_t *p, const lv_font_t *font, uint32_t color,
                          const char *text)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

/* Rounded panel that stacks its children vertically. */
static lv_obj_t *mk_panel(lv_obj_t *p)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(o, 14, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_hor(o, 20, 0);
    lv_obj_set_style_pad_ver(o, 14, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, 12, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* "Name ........ value" row inside a panel; returns the value label. */
static lv_obj_t *mk_row(lv_obj_t *panel, const char *name)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = mk_label(row, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, name);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = mk_label(row, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

/* Row of buttons where exactly one is selected (like radio buttons). */
static lv_obj_t *mk_choices(lv_obj_t *p, const char **map, int selected,
                            lv_coord_t height, lv_event_cb_t cb)
{
    lv_obj_t *m = lv_btnmatrix_create(p);
    lv_btnmatrix_set_map(m, map);
    lv_btnmatrix_set_btn_ctrl_all(m, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(m, true);
    if (selected >= 0) {
        lv_btnmatrix_set_btn_ctrl(m, selected, LV_BTNMATRIX_CTRL_CHECKED);
    }
    lv_obj_set_size(m, LV_PCT(100), height);

    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 12, 0);

    lv_obj_set_style_text_font(m, APP_FONT_MEDIUM, LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_hex(APP_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 12, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_BUTTON), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_ACCENT),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, lv_color_hex(0x0D1B2A),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return m;
}

/* Finger-sized checkbox; cb gets LV_EVENT_VALUE_CHANGED. */
static lv_obj_t *mk_checkbox(lv_obj_t *p, const char *text, bool checked, lv_event_cb_t cb)
{
    lv_obj_t *c = lv_checkbox_create(p);
    lv_checkbox_set_text(c, text);
    lv_obj_set_style_text_font(c, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_set_style_pad_column(c, 14, 0);
    lv_obj_set_style_pad_all(c, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_BUTTON), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(c, lv_color_hex(0x5F6B77), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(c, lv_color_hex(C_ACCENT),
                                  LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (checked) {
        lv_obj_add_state(c, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(c, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return c;
}

static bool is_checked(lv_event_t *e)
{
    return lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static lv_obj_t *mk_note(lv_obj_t *p, const char *text)
{
    lv_obj_t *n = mk_label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, text);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    return n;
}

static lv_obj_t *mk_tab(lv_obj_t *tv, const char *name)
{
    lv_obj_t *t = lv_tabview_add_tab(tv, name);
    lv_obj_set_style_pad_all(t, 18, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 14, 0);
    return t;
}

/* ---------- live values (updated by the timer) ---------- */

static const char *signal_word(int rssi)
{
    if (rssi >= -55) return "Excellent";
    if (rssi >= -67) return "Good";
    if (rssi >= -75) return "Fair";
    return "Weak";
}

static void update_network(void)
{
    char buf[64];

    if (!net_time_wifi_configured()) {
        lv_label_set_text(ui.net_status, "Not set up");
        lv_label_set_text(ui.net_ssid, "(edit wifi_secrets.h)");
        lv_label_set_text(ui.net_signal, "-");
        lv_label_set_text(ui.net_ip, "-");
        return;
    }

    wifi_ap_record_t ap;
    if (!net_time_wifi_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        lv_label_set_text(ui.net_status, LV_SYMBOL_CLOSE "  Not connected");
        lv_obj_set_style_text_color(ui.net_status, lv_color_hex(0xEF5350), 0);
        lv_label_set_text(ui.net_ssid, "-");
        lv_label_set_text(ui.net_signal, "-");
        lv_label_set_text(ui.net_ip, "-");
        return;
    }

    lv_label_set_text(ui.net_status, LV_SYMBOL_OK "  Connected");
    lv_obj_set_style_text_color(ui.net_status, lv_color_hex(0x66BB6A), 0);

    lv_label_set_text(ui.net_ssid, (const char *)ap.ssid);

    snprintf(buf, sizeof(buf), "%s  (%d dBm)", signal_word(ap.rssi), ap.rssi);
    lv_label_set_text(ui.net_signal, buf);

    esp_netif_ip_info_t ip;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        lv_label_set_text(ui.net_ip, buf);
    } else {
        lv_label_set_text(ui.net_ip, "-");
    }
}

static void update_time_preview(void)
{
    if (!net_time_is_synced()) {
        lv_label_set_text(ui.time_preview, "Example: waiting for the clock to sync");
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[16], buf[48];
    if (settings_get_24h()) {
        strftime(t, sizeof(t), "%H:%M", &tm);
        snprintf(buf, sizeof(buf), "Example: %s", t);
    } else {
        strftime(t, sizeof(t), "%I:%M %p", &tm);
        snprintf(buf, sizeof(buf), "Example: %s", t[0] == '0' ? t + 1 : t);
    }
    lv_label_set_text(ui.time_preview, buf);
}

static void update_time_source(void)
{
    char t[16], buf[48];
    time_t last = net_time_last_sync();

    switch (net_time_source()) {
    case TIME_SRC_INTERNET: {
        struct tm tm;
        localtime_r(&last, &tm);
        if (settings_get_24h()) {
            strftime(t, sizeof(t), "%H:%M", &tm);
        } else {
            strftime(t, sizeof(t), "%I:%M %p", &tm);
        }
        snprintf(buf, sizeof(buf), "Internet (synced %s)", t[0] == '0' ? t + 1 : t);
        lv_label_set_text(ui.time_source, buf);
        break;
    }
    case TIME_SRC_RTC:
        lv_label_set_text(ui.time_source, "Clock chip (until internet sync)");
        break;
    default:
        lv_label_set_text(ui.time_source, "Not set yet");
        break;
    }
}

static void update_about(void)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(ui.about_heap, buf);

    snprintf(buf, sizeof(buf), "%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_label_set_text(ui.about_psram, buf);

    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d) {
        snprintf(buf, sizeof(buf), "%" PRIu32 "d %" PRIu32 "h %02" PRIu32 "m", d, h, m);
    } else {
        snprintf(buf, sizeof(buf), "%" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s",
                 h, m, s % 60);
    }
    lv_label_set_text(ui.about_uptime, buf);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    update_network();
    update_time_preview();
    update_time_source();
    update_about();
}

/* ---------- event handlers ---------- */

static void timeout_changed(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id < sizeof(s_timeout_values) / sizeof(s_timeout_values[0])) {
        settings_set_screen_timeout_min(s_timeout_values[id]);
    }
}

static void fps_changed(lv_event_t *e)
{
    settings_set_show_fps(is_checked(e));
    launcher_apply_fps();
}

static void ticker_changed(lv_event_t *e)
{
    settings_set_ticker(is_checked(e));
    launcher_apply_ticker();
}

static void ticker_wx_changed(lv_event_t *e)
{
    settings_set_ticker_weather(is_checked(e));
}

static void ticker_cities_changed(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    uint32_t mask = 0;
    for (int i = 0; i < weather_location_count() && i < MAX_CITIES; i++) {
        if (lv_btnmatrix_has_btn_ctrl(m, i, LV_BTNMATRIX_CTRL_CHECKED)) {
            mask |= 1u << i;
        }
    }
    settings_set_ticker_cities(mask);
}

static void ticker_stocks_changed(lv_event_t *e)
{
    settings_set_ticker_stocks(is_checked(e));
}

static void clock_changed(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id <= 1) {
        settings_set_24h(id == 1);
        update_time_preview();
    }
}

static void city_changed(lv_event_t *e)
{
    uint16_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (id < weather_location_count()) {
        settings_set_weather_city(id);
    }
}

/* ---------- tabs ---------- */

static void build_network_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab);
    ui.net_status = mk_row(p, "Status");
    ui.net_ssid   = mk_row(p, "Network");
    ui.net_signal = mk_row(p, "Signal");
    ui.net_ip     = mk_row(p, "IP address");

    lv_obj_t *note = mk_label(tab, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                              "The network name and password are set in "
                              "main/wifi_secrets.h when the firmware is built.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

static void build_display_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab);
    mk_label(p, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Turn the screen off after");

    int sel = -1;
    for (int i = 0; i < (int)(sizeof(s_timeout_values) / sizeof(s_timeout_values[0])); i++) {
        if (s_timeout_values[i] == settings_get_screen_timeout_min()) {
            sel = i;
        }
    }
    mk_choices(p, s_timeout_map, sel, 70, timeout_changed);

    lv_obj_t *note = mk_label(p, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                              "The screen turns off when nobody has touched it for "
                              "this long. Tap anywhere to wake it; that tap won't "
                              "press any button. (This screen has no brightness "
                              "control, only on and off.)");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    lv_obj_t *tp = mk_panel(tab);
    mk_checkbox(tp, "Show the info ticker on the home screen", settings_get_ticker(),
                ticker_changed);
    mk_note(tp, "Scrolling weather and stock updates along the bottom of the home "
                "screen, kept up to date in the background. Choose what it shows "
                "in the Weather and St0nks tabs.");

    lv_obj_t *fp = mk_panel(tab);
    mk_checkbox(fp, "Show FPS / CPU meter", settings_get_show_fps(), fps_changed);
    mk_note(fp, "Frames per second and how busy the drawing task is, shown in "
                "the bottom-right corner.");
}

static void build_time_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab);
    mk_label(p, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Clock format");
    mk_choices(p, s_clock_map, settings_get_24h() ? 1 : 0, 70, clock_changed);
    ui.time_preview = mk_label(p, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");

    lv_obj_t *src = mk_panel(tab);
    ui.time_source = mk_row(src, "Time source");

    lv_obj_t *note = mk_label(tab, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                              "Used by the home screen clock and World Clock. "
                              "The UTC card always shows 24-hour time. Each "
                              "internet sync is saved to the clock chip, so the "
                              "time is right at power-on even without Wi-Fi.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
}

static void build_weather_tab(lv_obj_t *tab)
{
    int n = weather_location_count();
    if (n > MAX_CITIES) {
        n = MAX_CITIES;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i > 0 && i % 2 == 0) {
            s_city_map[k++] = "\n";
        }
        s_city_map[k++] = weather_location_name(i);
    }
    s_city_map[k] = "";

    lv_obj_t *p = mk_panel(tab);
    mk_label(p, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Weather opens to");
    int rows = (n + 1) / 2;
    mk_choices(p, s_city_map, settings_get_weather_city(), rows * 62 + (rows - 1) * 12,
               city_changed);

    /* Info ticker: same buttons, but any number can be on. */
    lv_obj_t *tp = mk_panel(tab);
    mk_checkbox(tp, "Show weather in the info ticker", settings_get_ticker_weather(),
                ticker_wx_changed);
    mk_label(tp, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Cities in the ticker");
    lv_obj_t *m = mk_choices(tp, s_city_map, -1, rows * 62 + (rows - 1) * 12,
                             ticker_cities_changed);
    lv_btnmatrix_set_one_checked(m, false);
    uint32_t mask = settings_get_ticker_cities();
    for (int i = 0; i < n; i++) {
        if (mask & (1u << i)) {
            lv_btnmatrix_set_btn_ctrl(m, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }
    mk_note(tp, "Each city shows the current weather and the forecast for today "
                "and tomorrow. "
                "Turn the ticker itself on in the Display tab.");
}

static void build_stonks_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab);
    mk_checkbox(p, "Show my watchlist in the info ticker", settings_get_ticker_stocks(),
                ticker_stocks_changed);
    mk_note(p, "Each symbol's price and today's change. Add or remove symbols in "
               "the St0nks app. Turn the ticker itself on in the Display tab.");
}

static void build_about_tab(lv_obj_t *tab)
{
    char buf[48];
    lv_obj_t *p = mk_panel(tab);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    snprintf(buf, sizeof(buf), "ESP32-S3 rev v%d.%d, %d cores",
             chip.revision / 100, chip.revision % 100, chip.cores);
    lv_label_set_text(mk_row(p, "Chip"), buf);

    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    snprintf(buf, sizeof(buf), "%" PRIu32 " MB", flash / (1024 * 1024));
    lv_label_set_text(mk_row(p, "Flash"), buf);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    lv_label_set_text(mk_row(p, "Wi-Fi MAC"), buf);

    lv_label_set_text(mk_row(p, "ESP-IDF"), esp_get_idf_version());

    snprintf(buf, sizeof(buf), "%d.%d.%d",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_label_set_text(mk_row(p, "LVGL"), buf);

    ui.about_heap   = mk_row(p, "Free internal RAM");
    ui.about_psram  = mk_row(p, "Free PSRAM");
    ui.about_uptime = mk_row(p, "Uptime");
}

/* ---------- app entry points ---------- */

static void settings_create(lv_obj_t *parent)
{
    memset(&ui, 0, sizeof(ui));

    lv_obj_t *tv = lv_tabview_create(parent, LV_DIR_TOP, TAB_H);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tv, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);

    /* Tab buttons: dark bar, blue text + underline on the selected tab. */
    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(btns, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_text_font(btns, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(btns, lv_color_hex(APP_COLOR_SUBTEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_color(btns, lv_color_hex(C_ACCENT),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btns, lv_color_hex(C_ACCENT),
                                  LV_PART_ITEMS | LV_STATE_CHECKED);

    /* Change tabs by tapping only; swiping sideways is too easy to trigger. */
    lv_obj_t *content = lv_tabview_get_content(tv);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(content, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    build_network_tab(mk_tab(tv, LV_SYMBOL_WIFI " Network"));
    build_display_tab(mk_tab(tv, LV_SYMBOL_EYE_OPEN " Display"));
    build_time_tab(mk_tab(tv, LV_SYMBOL_BELL " Time"));
    build_weather_tab(mk_tab(tv, LV_SYMBOL_HOME " Weather"));
    build_stonks_tab(mk_tab(tv, LV_SYMBOL_UP " St0nks"));
    build_about_tab(mk_tab(tv, LV_SYMBOL_LIST " About"));

    tick(NULL);
    s_timer = lv_timer_create(tick, 1000, NULL);
}

static void settings_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    memset(&ui, 0, sizeof(ui));
}

const app_t settings_app = {
    .name      = "Settings",
    .symbol    = LV_SYMBOL_SETTINGS,
    .color_hex = 0x546E7A,
    .create    = settings_create,
    .destroy   = settings_destroy,
    .pin_last  = true,
};
