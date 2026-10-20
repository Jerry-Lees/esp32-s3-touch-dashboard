/*
 * USB keyboard (TinyUSB HID) on the shared USB port; see usb_kbd.h.
 *
 * One HID interface with two reports: 1 = keyboard (modifiers + up to 6
 * keys), 2 = consumer control (media keys). Combos are "+"-separated,
 * case-insensitive:
 *   modifiers  ctrl shift alt meta (also super, win, gui)
 *   keys       a-z 0-9 f1-f24 enter esc tab space backspace delete insert
 *              home end pageup pagedown up down left right print minus
 *              equal comma period slash semicolon
 *   media      play next prev stop mute volup voldown (on their own)
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hal/usb_serial_jtag_ll.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

#include "usb_kbd.h"

static const char *TAG = "usb_kbd";

enum { RID_KEYBOARD = 1, RID_CONSUMER = 2 };

static const uint8_t s_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
};

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_report_desc), 0x81, 16, 10),
};

static const char *s_strings[] = {
    (const char[]){ 0x09, 0x04 },     /* English */
    "ESP32 Dashboard",                /* manufacturer */
    "DashPad",                        /* product */
    "1",                              /* serial */
    "DashPad keys",                   /* HID interface */
};

static bool s_active;

/* TinyUSB callbacks */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                               uint8_t *buf, uint16_t len)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t id, hid_report_type_t type,
                           uint8_t const *buf, uint16_t len)
{
}

/* ======================= the port ======================= */

static void port_to_serial_jtag(void)
{
    /* Internal USB PHY -> USB-Serial/JTAG (what it is after power-on). */
    usb_serial_jtag_ll_phy_enable_external(false);
}

void usb_kbd_boot_fix(void)
{
    port_to_serial_jtag();
}

bool usb_kbd_start(void)
{
    if (s_active) return true;
    const tinyusb_config_t cfg = {
        .device_descriptor = NULL,           /* Espressif VID, TinyUSB's PID (Kconfig) */
        .string_descriptor = s_strings,
        .string_descriptor_count = sizeof(s_strings) / sizeof(s_strings[0]),
        .external_phy = false,
        .configuration_descriptor = s_config_desc,
    };
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Couldn't start the USB keyboard: %s", esp_err_to_name(err));
        port_to_serial_jtag();
        return false;
    }
    s_active = true;
    return true;
}

void usb_kbd_stop(void)
{
    if (!s_active) return;
    esp_err_t err = tinyusb_driver_uninstall();
    if (err != ESP_OK) ESP_LOGW(TAG, "Stopping the USB keyboard: %s", esp_err_to_name(err));
    port_to_serial_jtag();
    s_active = false;
}

bool usb_kbd_active(void) { return s_active; }
bool usb_kbd_ready(void) { return s_active && tud_mounted() && tud_hid_ready(); }

/* ======================= combos ======================= */

static const struct { const char *name; uint8_t mod; } MODS[] = {
    { "ctrl", KEYBOARD_MODIFIER_LEFTCTRL }, { "control", KEYBOARD_MODIFIER_LEFTCTRL },
    { "shift", KEYBOARD_MODIFIER_LEFTSHIFT }, { "alt", KEYBOARD_MODIFIER_LEFTALT },
    { "meta", KEYBOARD_MODIFIER_LEFTGUI }, { "super", KEYBOARD_MODIFIER_LEFTGUI },
    { "win", KEYBOARD_MODIFIER_LEFTGUI }, { "gui", KEYBOARD_MODIFIER_LEFTGUI },
};

static const struct { const char *name; uint8_t key; } KEYS[] = {
    { "enter", HID_KEY_ENTER }, { "return", HID_KEY_ENTER }, { "esc", HID_KEY_ESCAPE },
    { "escape", HID_KEY_ESCAPE }, { "tab", HID_KEY_TAB }, { "space", HID_KEY_SPACE },
    { "backspace", HID_KEY_BACKSPACE }, { "delete", HID_KEY_DELETE }, { "del", HID_KEY_DELETE },
    { "insert", HID_KEY_INSERT }, { "home", HID_KEY_HOME }, { "end", HID_KEY_END },
    { "pageup", HID_KEY_PAGE_UP }, { "pagedown", HID_KEY_PAGE_DOWN },
    { "up", HID_KEY_ARROW_UP }, { "down", HID_KEY_ARROW_DOWN },
    { "left", HID_KEY_ARROW_LEFT }, { "right", HID_KEY_ARROW_RIGHT },
    { "print", HID_KEY_PRINT_SCREEN }, { "minus", HID_KEY_MINUS }, { "equal", HID_KEY_EQUAL },
    { "comma", HID_KEY_COMMA }, { "period", HID_KEY_PERIOD }, { "slash", HID_KEY_SLASH },
    { "semicolon", HID_KEY_SEMICOLON },
};

static const struct { const char *name; uint16_t usage; } MEDIA[] = {
    { "play", HID_USAGE_CONSUMER_PLAY_PAUSE }, { "next", HID_USAGE_CONSUMER_SCAN_NEXT_TRACK },
    { "prev", HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK }, { "stop", HID_USAGE_CONSUMER_STOP },
    { "mute", HID_USAGE_CONSUMER_MUTE }, { "volup", HID_USAGE_CONSUMER_VOLUME_INCREMENT },
    { "voldown", HID_USAGE_CONSUMER_VOLUME_DECREMENT },
};

typedef struct {
    uint8_t  mods;
    uint8_t  keys[6];
    int      nkeys;
    uint16_t media;
} combo_t;

static bool key_code(const char *t, uint8_t *code)
{
    size_t n = strlen(t);
    if (n == 1 && isalpha((unsigned char)t[0])) {
        *code = HID_KEY_A + (tolower((unsigned char)t[0]) - 'a');
        return true;
    }
    if (n == 1 && isdigit((unsigned char)t[0])) {
        *code = t[0] == '0' ? HID_KEY_0 : HID_KEY_1 + (t[0] - '1');
        return true;
    }
    if ((t[0] == 'f' || t[0] == 'F') && n >= 2 && n <= 3 && isdigit((unsigned char)t[1])) {
        int f = atoi(t + 1);
        if (f >= 1 && f <= 12) { *code = HID_KEY_F1 + (f - 1); return true; }
        if (f >= 13 && f <= 24) { *code = HID_KEY_F13 + (f - 13); return true; }
    }
    for (size_t i = 0; i < sizeof(KEYS) / sizeof(KEYS[0]); i++) {
        if (!strcasecmp(KEYS[i].name, t)) { *code = KEYS[i].key; return true; }
    }
    return false;
}

static bool parse(const char *combo, combo_t *c, char *why, size_t wl)
{
    memset(c, 0, sizeof(*c));
    char buf[64];
    if (!combo || !*combo || strlen(combo) >= sizeof(buf)) {
        snprintf(why, wl, "enter keys such as ctrl+alt+t");
        return false;
    }
    snprintf(buf, sizeof(buf), "%s", combo);
    char *save;
    for (char *t = strtok_r(buf, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        while (*t == ' ') t++;
        for (char *e = t + strlen(t); e > t && e[-1] == ' '; ) *--e = '\0';
        bool found = false;
        for (size_t i = 0; !found && i < sizeof(MODS) / sizeof(MODS[0]); i++) {
            if (!strcasecmp(MODS[i].name, t)) { c->mods |= MODS[i].mod; found = true; }
        }
        for (size_t i = 0; !found && i < sizeof(MEDIA) / sizeof(MEDIA[0]); i++) {
            if (!strcasecmp(MEDIA[i].name, t)) { c->media = MEDIA[i].usage; found = true; }
        }
        uint8_t code;
        if (!found && key_code(t, &code)) {
            if (c->nkeys == 6) {
                snprintf(why, wl, "at most 6 keys at once");
                return false;
            }
            c->keys[c->nkeys++] = code;
            found = true;
        }
        if (!found) {
            snprintf(why, wl, "unknown key \"%s\"", t);
            return false;
        }
    }
    if (c->media && (c->mods || c->nkeys)) {
        snprintf(why, wl, "media keys (play, mute, volup...) go on their own");
        return false;
    }
    if (!c->media && !c->nkeys) {
        snprintf(why, wl, "a combo needs a key, not only modifiers");
        return false;
    }
    return true;
}

bool usb_kbd_parse(const char *combo, char *why, size_t wl)
{
    combo_t c;
    return parse(combo, &c, why, wl);
}

static bool wait_ready(void)
{
    for (int i = 0; i < 50; i++) {                  /* up to 1 s */
        if (usb_kbd_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

bool usb_kbd_send(const char *combo, char *why, size_t wl)
{
    combo_t c;
    if (!parse(combo, &c, why, wl)) return false;
    if (!s_active) {
        snprintf(why, wl, "The USB keyboard is off: turn it on in DashPad's menu");
        return false;
    }
    if (!wait_ready()) {
        snprintf(why, wl, "The PC hasn't set up the USB keyboard (is the cable in?)");
        return false;
    }
    if (c.media) {
        tud_hid_report(RID_CONSUMER, &c.media, 2);
        vTaskDelay(pdMS_TO_TICKS(40));
        wait_ready();
        uint16_t none = 0;
        tud_hid_report(RID_CONSUMER, &none, 2);
    } else {
        tud_hid_keyboard_report(RID_KEYBOARD, c.mods, c.keys);
        vTaskDelay(pdMS_TO_TICKS(40));
        wait_ready();
        tud_hid_keyboard_report(RID_KEYBOARD, 0, NULL);
    }
    return true;
}
