#pragma once
/*
 * microSD card on SPI (MOSI GPIO11, CLK GPIO12, MISO GPIO13). The card's
 * CS is CH422G EXIO4, held low permanently by waveshare_rgb_lcd_port.c.
 * Mounted at /sdcard in a background task at boot (no hot-plug: insert the
 * card, then restart). FAT32 or exFAT; ESP-IDF
 * must have exFAT enabled (README.md, "exFAT support") for cards over 32 GB.
 */
#include <stdbool.h>
#include <stddef.h>

#define SD_MOUNT_POINT "/sdcard"
#define SD_DIR         SD_MOUNT_POINT "/dashboard"   /* everything of ours lives here */

/* Starts the background task that mounts the card, creates the /dashboard
 * folders and imports /dashboard/import/wifi.json (then erases it). Call
 * after wifi_list_init() and net_time_start(). */
void sd_card_start(void);

/* A message for the screen (e.g. "Imported 2 Wi-Fi networks"), at most
 * once: copies it into out and returns true if there's a new one. */
bool sd_card_take_notice(char *out, size_t len);

/* True once the card is mounted and files can be read. */
bool sd_card_mounted(void);
