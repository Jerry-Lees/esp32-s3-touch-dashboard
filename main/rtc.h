#pragma once
/*
 * PCF85063 real-time clock on the shared I2C bus (address 0x51).
 * The chip keeps UTC; time zones are applied by the system as usual.
 *
 * The I2C bus must already be set up (waveshare_esp32_s3_rgb_lcd_init()).
 * Safe to call from any task: the legacy I2C driver serializes access.
 */
#include <stdbool.h>
#include <time.h>

/* Reads the chip. Returns false if it can't be reached, lost power since it
 * was last set, or holds an implausible date. */
bool rtc_read_utc(time_t *out);

/* Sets the chip and clears its "lost power" flag. */
bool rtc_write_utc(time_t t);
