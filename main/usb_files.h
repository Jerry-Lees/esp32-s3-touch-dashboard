#pragma once
/*
 * SD card file transfer over the USB cable (see usb_files.c for the
 * protocol; the PC side is tools/sdcard.py). Only the dashboard folder on
 * the card can be reached.
 */

/* Installs the USB-Serial/JTAG driver (logs keep working through it) and
 * starts the command task. Call once from app_main. */
void usb_files_start(void);
