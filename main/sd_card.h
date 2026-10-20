#pragma once
/*
 * microSD card on SPI (MOSI GPIO11, CLK GPIO12, MISO GPIO13). The card's
 * CS is CH422G EXIO4, held low permanently by waveshare_rgb_lcd_port.c.
 * Mounted at /sdcard in a background task at boot. FAT32 or exFAT; ESP-IDF
 * must have exFAT enabled (README.md, "exFAT support") for cards over 32 GB.
 */
#include <stdbool.h>

#define SD_MOUNT_POINT "/sdcard"

/* Starts the background task that mounts the card. */
void sd_card_start(void);

/* True once the card is mounted and files can be read. */
bool sd_card_mounted(void);
