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
#include "game_common.h"
#include "launcher.h"
#include "net_time.h"
#include "settings.h"
#include "wifi_list.h"

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
    lv_obj_t *root;
    lv_obj_t *net_status, *net_ssid, *net_signal, *net_ip;
    lv_obj_t *net_title, *net_list;
    uint32_t  net_version;
    char      net_shown[WIFI_SSID_MAX + 1];   /* connected network when the list was drawn */
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

static void rebuild_net_list(void);

static void update_network(void)
{
    char buf[64];

    /* Redraw the saved list when it changes or we connect elsewhere. */
    char cur[WIFI_SSID_MAX + 1] = "";
    wifi_ap_record_t cur_ap;
    if (net_time_wifi_connected() && esp_wifi_sta_get_ap_info(&cur_ap) == ESP_OK) {
        strlcpy(cur, (const char *)cur_ap.ssid, sizeof(cur));
    }
    if (ui.net_list && (wifi_list_version() != ui.net_version || strcmp(cur, ui.net_shown) != 0)) {
        rebuild_net_list();
    }

    if (!net_time_wifi_configured()) {
        lv_label_set_text(ui.net_status, "Not set up");
        lv_label_set_text(ui.net_ssid, "(no saved networks)");
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

/* ---------- saved networks ---------- */

static char s_row_ssid[WIFI_LIST_MAX][WIFI_SSID_MAX + 1];
static char s_remove_ssid[WIFI_SSID_MAX + 1];

static void remove_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (!yes) return;
    if (wifi_list_remove(s_remove_ssid)) {
        net_time_networks_changed();     /* drops the connection if it was this one */
        char msg[80];
        snprintf(msg, sizeof(msg), "Removed \"%s\"", s_remove_ssid);
        launcher_toast(msg, 4000);
    }
    rebuild_net_list();
}

static void wipe_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (!yes) return;
    bool ok = wifi_list_wipe();
    net_time_networks_changed();         /* drop a connection to a wiped network */
    launcher_toast(ok ? "All saved networks were wiped from the display's memory"
                      : "Wipe failed (see the log); some settings may be reset",
                   ok ? 5000 : 10000);
    rebuild_net_list();
}

static void wipe_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *row = game_dialog_open(ui.root, "Wipe all saved networks?\n"
                                              "They're erased from memory for good,\n"
                                              "including ones removed earlier.\n"
                                              "Other settings are kept.");
    game_dialog_button(row, LV_SYMBOL_TRASH "  Wipe", wipe_answer, (void *)1);
    game_dialog_button(row, "Cancel", wipe_answer, NULL);
}

static void remove_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_remove_ssid, s_row_ssid[i], sizeof(s_remove_ssid));
    char title[64];
    snprintf(title, sizeof(title), "Forget \"%s\"?", s_remove_ssid);
    lv_obj_t *row = game_dialog_open(ui.root, title);
    game_dialog_button(row, LV_SYMBOL_TRASH "  Remove", remove_answer, (void *)1);
    game_dialog_button(row, "Cancel", remove_answer, NULL);
}

/* One network: name, a "Connected"/"Built-in" tag, and Remove (index >= 0). */
static void mk_net_row(const char *ssid, bool connected, const char *tag, int index)
{
    lv_obj_t *r = lv_obj_create(ui.net_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), 56);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_set_style_bg_color(r, lv_color_hex(0x232D36), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(r, 14, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *n = mk_label(r, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_text_fmt(n, LV_SYMBOL_WIFI "  %s", ssid);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_width(n, 380);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

    const char *t = connected ? LV_SYMBOL_OK " Connected" : tag;
    if (t) {
        lv_obj_t *tl = mk_label(r, LV_FONT_DEFAULT, connected ? 0x66BB6A : APP_COLOR_SUBTEXT, t);
        lv_obj_align(tl, LV_ALIGN_RIGHT_MID, index >= 0 ? -150 : 0, 0);
    }

    if (index >= 0) {
        strlcpy(s_row_ssid[index], ssid, sizeof(s_row_ssid[index]));
        lv_obj_t *b = lv_btn_create(r);
        lv_obj_set_size(b, 136, 42);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x5D1F1F), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, remove_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);
        lv_obj_t *bl = mk_label(b, APP_FONT_MEDIUM, APP_COLOR_TEXT, LV_SYMBOL_TRASH "  Remove");
        lv_obj_center(bl);
    }
}

static void rebuild_net_list(void)
{
    ui.net_version = wifi_list_version();
    ui.net_shown[0] = '\0';
    wifi_ap_record_t ap;
    if (net_time_wifi_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(ui.net_shown, (const char *)ap.ssid, sizeof(ui.net_shown));
    }

    lv_obj_clean(ui.net_list);
    int n = wifi_list_count();
    const char *b = net_time_builtin_ssid();
    bool show_builtin = b && !wifi_list_contains(b);
    /* The built-in network doesn't use one of the WIFI_LIST_MAX slots. */
    if (show_builtin) {
        lv_label_set_text_fmt(ui.net_title, "Saved networks: %d of %d  (+ 1 built-in)",
                              n, WIFI_LIST_MAX);
    } else {
        lv_label_set_text_fmt(ui.net_title, "Saved networks: %d of %d", n, WIFI_LIST_MAX);
    }
    char ssid[WIFI_SSID_MAX + 1];
    for (int i = 0; i < n; i++) {
        if (wifi_list_ssid(i, ssid, sizeof(ssid))) {
            mk_net_row(ssid, strcmp(ssid, ui.net_shown) == 0, NULL, i);
        }
    }
    /* The wifi_secrets.h network can't be removed here; a saved network
     * with the same name takes its place. */
    if (show_builtin) {
        mk_net_row(b, strcmp(b, ui.net_shown) == 0, "Built-in", -1);
    }
    if (n == 0 && !b) {
        mk_label(ui.net_list, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "No networks yet.");
    }
}

/* ---------- adding a network on screen ----------
 * A full-screen overlay over the Settings app with two pages: a scan list
 * (tap a network), then the password with the on-screen keyboard.
 */

static struct {
    lv_obj_t   *view, *pick, *pass;          /* overlay and its two pages */
    lv_obj_t   *status, *list;               /* pick page */
    lv_obj_t   *title, *ta, *msg, *kb;       /* password page */
    lv_timer_t *poll;
    net_scan_ap_t aps[NET_SCAN_MAX];
    int         n;
    int         chosen;                      /* index into aps */
} add;

static const char *auth_word(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:            return "Open";
    case WIFI_AUTH_WEP:             return "WEP (not supported)";
    case WIFI_AUTH_WPA2_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE: return "Enterprise (not supported)";
    default:                        return "Secured";
    }
}

static bool auth_supported(wifi_auth_mode_t a)
{
    return strstr(auth_word(a), "not supported") == NULL;
}

static void add_close(void)
{
    if (add.poll) lv_timer_del(add.poll);
    if (add.ta) lv_textarea_set_text(add.ta, "");
    if (add.view) lv_obj_del_async(add.view);   /* we may be inside its event */
    memset(&add, 0, sizeof(add));
}

static void add_cancel_clicked(lv_event_t *e)
{
    (void)e;
    add_close();
}

static void add_show_pick(void)
{
    lv_obj_clear_flag(add.pick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(add.pass, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(add.ta, "");
}

static void add_save(void)
{
    const net_scan_ap_t *ap = &add.aps[add.chosen];
    bool open = ap->auth == WIFI_AUTH_OPEN;
    const char *pw = open ? "" : lv_textarea_get_text(add.ta);
    size_t len = strlen(pw);
    if (!open && !(len >= 8 && len <= 63)) {
        lv_label_set_text(add.msg, "The password must be 8 to 63 characters.");
        return;
    }
    if (!wifi_list_contains(ap->ssid) && wifi_list_count() >= WIFI_LIST_MAX) {
        lv_label_set_text_fmt(add.msg, "You already have %d saved networks. Remove one "
                              "first.", WIFI_LIST_MAX);
        return;
    }
    wifi_net_t net = {0};
    strlcpy(net.ssid, ap->ssid, sizeof(net.ssid));
    strlcpy(net.pass, pw, sizeof(net.pass));
    bool ok = wifi_list_add(&net, 1);
    memset(&net, 0, sizeof(net));
    if (!ok) {
        lv_label_set_text(add.msg, "Saving failed. Try again.");
        return;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Saved \"%s\"", ap->ssid);
    net_time_networks_changed();             /* connect if we aren't yet */
    add_close();
    launcher_toast(msg, 4000);
    rebuild_net_list();
}

static void add_save_clicked(lv_event_t *e)
{
    (void)e;
    add_save();
}

static void add_back_clicked(lv_event_t *e)
{
    (void)e;
    add_show_pick();
}

static void add_kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) add_save();
    else add_show_pick();
}

static void add_show_clicked(lv_event_t *e)
{
    lv_textarea_set_password_mode(add.ta, !is_checked(e));
}

static void add_ap_clicked(lv_event_t *e)
{
    add.chosen = (int)(intptr_t)lv_event_get_user_data(e);
    const net_scan_ap_t *ap = &add.aps[add.chosen];
    bool open = ap->auth == WIFI_AUTH_OPEN;
    lv_label_set_text_fmt(add.title, open ? "Save the open network \"%s\"?"
                                          : "Password for \"%s\"", ap->ssid);
    lv_label_set_text(add.msg, open ? "This network has no password. Anyone nearby can "
                                      "see what's sent over it." : "");
    if (open) {
        lv_obj_add_flag(add.ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(add.kb, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(add.ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(add.kb, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(add.pick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(add.pass, LV_OBJ_FLAG_HIDDEN);
}

static void add_fill_list(void)
{
    lv_obj_clean(add.list);
    if (add.n == 0) {
        lv_label_set_text(add.status, "No networks found. Tap Rescan to try again.");
        return;
    }
    lv_label_set_text(add.status, "Tap a network to add it (2.4 GHz networks only):");
    for (int i = 0; i < add.n; i++) {
        const net_scan_ap_t *ap = &add.aps[i];
        bool ok = auth_supported(ap->auth);
        lv_obj_t *r = lv_btn_create(add.list);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x232D36), 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        if (ok) lv_obj_add_event_cb(r, add_ap_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        else lv_obj_add_state(r, LV_STATE_DISABLED);

        lv_obj_t *n = mk_label(r, APP_FONT_MEDIUM, ok ? APP_COLOR_TEXT : APP_COLOR_SUBTEXT, "");
        lv_label_set_text_fmt(n, LV_SYMBOL_WIFI "  %s", ap->ssid);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_width(n, 400);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

        char info[64];
        snprintf(info, sizeof(info), "%s%s  -  %s", wifi_list_contains(ap->ssid) ?
                 "Saved  -  " : "", auth_word(ap->auth), signal_word(ap->rssi));
        lv_obj_t *t = mk_label(r, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, info);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void add_poll(lv_timer_t *t)
{
    (void)t;
    int n = net_time_scan_results(add.aps, NET_SCAN_MAX);
    if (n < 0) return;                           /* still scanning */
    add.n = n;
    lv_timer_pause(add.poll);
    add_fill_list();
}

static void add_rescan(void)
{
    lv_obj_clean(add.list);
    lv_label_set_text(add.status, LV_SYMBOL_REFRESH "  Looking for networks...");
    net_time_scan_request();
    lv_timer_resume(add.poll);
}

static void add_rescan_clicked(lv_event_t *e)
{
    (void)e;
    add_rescan();
}

static lv_obj_t *mk_btn(lv_obj_t *p, const char *text, int w, lv_event_cb_t cb, uint32_t color)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, 48);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(b, APP_FONT_MEDIUM, APP_COLOR_TEXT, text));
    return b;
}

static void add_clicked(lv_event_t *e)
{
    (void)e;
    if (add.view) return;
    lv_obj_t *v = add.view = lv_obj_create(ui.root);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(v, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_add_flag(v, LV_OBJ_FLAG_CLICKABLE);        /* blocks the tabs underneath */
    lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);

    /* Page 1: pick a network. */
    lv_obj_t *pk = add.pick = lv_obj_create(v);
    lv_obj_remove_style_all(pk);
    lv_obj_set_size(pk, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(pk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = mk_label(pk, APP_FONT_LARGE, APP_COLOR_TEXT, "Add a Wi-Fi network");
    lv_obj_set_pos(t, 16, 14);
    lv_obj_align(mk_btn(pk, LV_SYMBOL_REFRESH "  Rescan", 150, add_rescan_clicked, C_BUTTON),
                 LV_ALIGN_TOP_RIGHT, -140, 8);
    lv_obj_align(mk_btn(pk, "Cancel", 116, add_cancel_clicked, C_BUTTON),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    add.status = mk_label(pk, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_set_pos(add.status, 16, 62);
    add.list = lv_obj_create(pk);
    lv_obj_remove_style_all(add.list);
    lv_obj_set_size(add.list, LV_PCT(100), 416 - 88);
    lv_obj_set_pos(add.list, 0, 88);
    lv_obj_set_style_pad_hor(add.list, 12, 0);
    lv_obj_set_style_pad_bottom(add.list, 12, 0);
    lv_obj_set_style_pad_row(add.list, 8, 0);
    lv_obj_set_flex_flow(add.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(add.list, LV_DIR_VER);

    /* Page 2: password. */
    lv_obj_t *pp = add.pass = lv_obj_create(v);
    lv_obj_remove_style_all(pp);
    lv_obj_set_size(pp, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(pp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pp, LV_OBJ_FLAG_HIDDEN);
    add.title = mk_label(pp, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_long_mode(add.title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(add.title, 460);
    lv_obj_set_pos(add.title, 16, 18);
    lv_obj_align(mk_btn(pp, LV_SYMBOL_OK "  Save", 130, add_save_clicked, 0x1B8F4A),
                 LV_ALIGN_TOP_RIGHT, -140, 8);
    lv_obj_align(mk_btn(pp, LV_SYMBOL_LEFT "  Back", 116, add_back_clicked, C_BUTTON),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    add.ta = lv_textarea_create(pp);
    lv_textarea_set_one_line(add.ta, true);
    lv_textarea_set_password_mode(add.ta, true);
    lv_textarea_set_max_length(add.ta, 63);
    lv_textarea_set_placeholder_text(add.ta, "Password");
    lv_obj_set_style_text_font(add.ta, APP_FONT_MEDIUM, 0);
    lv_obj_set_size(add.ta, 560, 52);
    lv_obj_set_pos(add.ta, 16, 66);
    lv_obj_add_state(add.ta, LV_STATE_FOCUSED);       /* show the cursor */
    lv_obj_t *show = mk_checkbox(pp, "Show", false, add_show_clicked);
    lv_obj_set_pos(show, 600, 76);
    add.msg = mk_label(pp, LV_FONT_DEFAULT, 0xFFB74D, "");
    lv_label_set_long_mode(add.msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(add.msg, 768);
    lv_obj_set_pos(add.msg, 16, 128);
    add.kb = lv_keyboard_create(pp);
    lv_obj_set_size(add.kb, 800, 250);
    lv_obj_align(add.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(add.kb, add.ta);
    lv_obj_add_event_cb(add.kb, add_kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(add.kb, add_kb_event, LV_EVENT_CANCEL, NULL);

    add.poll = lv_timer_create(add_poll, 250, NULL);
    add_rescan();
}

static void build_network_tab(lv_obj_t *tab)
{
    lv_obj_t *p = mk_panel(tab);
    ui.net_status = mk_row(p, "Status");
    ui.net_ssid   = mk_row(p, "Network");
    ui.net_signal = mk_row(p, "Signal");
    ui.net_ip     = mk_row(p, "IP address");

    lv_obj_t *sp = mk_panel(tab);
    ui.net_title = mk_label(sp, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Saved networks");
    ui.net_list = lv_obj_create(sp);
    lv_obj_remove_style_all(ui.net_list);
    lv_obj_set_size(ui.net_list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ui.net_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui.net_list, 8, 0);
    lv_obj_clear_flag(ui.net_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ab = mk_btn(sp, LV_SYMBOL_PLUS "  Add network", 220, add_clicked, 0x1E5A8C);
    (void)ab;
    mk_note(sp, "The display connects to the strongest saved network in range. Add "
                "networks here, or several at once with a wifi.json file in the "
                "dashboard/import folder on the SD card (see README.md; the file is erased "
                "once imported). Built-in = set in main/wifi_secrets.h when the firmware "
                "was built.");

    /* Remove only marks a network as deleted in flash; this erases it. */
    lv_obj_t *wp = mk_panel(tab);
    lv_obj_t *wb = lv_btn_create(wp);
    lv_obj_set_size(wb, LV_PCT(100), 56);
    lv_obj_set_style_radius(wb, 12, 0);
    lv_obj_set_style_bg_color(wb, lv_color_hex(0x5D1F1F), 0);
    lv_obj_set_style_shadow_width(wb, 0, 0);
    lv_obj_add_event_cb(wb, wipe_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(wb, APP_FONT_MEDIUM, APP_COLOR_TEXT,
                           LV_SYMBOL_TRASH "  Securely wipe all saved networks"));
    mk_note(wp, "Remove takes a network off the list straight away, but its password "
                "stays in the display's flash memory, marked as deleted, until that space "
                "is reused. Wiping erases the settings area completely, so every saved and "
                "removed network is gone for good. Your other settings are copied and put "
                "back. The built-in network isn't affected.");
    rebuild_net_list();
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
    ui.root = parent;

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
    game_dialog_close();
    if (add.poll) lv_timer_del(add.poll);
    memset(&add, 0, sizeof(add));            /* the launcher deletes the objects */
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
