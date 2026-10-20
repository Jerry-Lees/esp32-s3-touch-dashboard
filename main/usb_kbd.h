#pragma once
/*
 * USB keyboard for DashPad. The ESP32-S3's USB-C port normally belongs to
 * the USB-Serial/JTAG unit (flashing, the log, tools/sdcard.py). While
 * DashPad is open with keyboard buttons, the port is switched to the USB
 * OTG controller running a TinyUSB keyboard ("DashPad"); closing DashPad
 * switches it back. Only one of the two can use the port at a time.
 *
 * The switch is a register that survives a software restart, so
 * usb_kbd_boot_fix() hands the port back to Serial/JTAG at every start-up.
 */
#include <stdbool.h>
#include <stddef.h>

/* Very first thing at start-up (before the USB-Serial/JTAG driver). */
void usb_kbd_boot_fix(void);

/* LVGL task. Switch the port to the keyboard / back to Serial/JTAG. */
bool usb_kbd_start(void);
void usb_kbd_stop(void);
bool usb_kbd_active(void);
/* True once the PC has set the keyboard up (it can take a second). */
bool usb_kbd_ready(void);

/* Checks a key combo such as "ctrl+alt+t", "meta+l", "f13" or "volup"
 * (see usb_kbd.c for the names). False with why if it can't be sent. */
bool usb_kbd_parse(const char *combo, char *why, size_t why_len);
/* Presses and releases the combo (any task). False with why on failure. */
bool usb_kbd_send(const char *combo, char *why, size_t why_len);
