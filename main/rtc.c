/*
 * PCF85063 driver: just enough to save and restore the time.
 *
 * Registers 0x04..0x0A hold seconds, minutes, hours, day, weekday, month
 * and year in BCD. Bit 7 of the seconds register (OS) is set by the chip
 * when its oscillator stopped, i.e. it lost power and the time is junk.
 * The year register holds 0-99; we store years 2000-2099.
 */
#include <stdint.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "rtc.h"

static const char *TAG = "rtc";

#define RTC_I2C_PORT    0       /* same bus as touch and CH422G */
#define RTC_ADDR        0x51
#define RTC_REG_SECONDS 0x04
#define RTC_OS_FLAG     0x80
#define RTC_TIMEOUT     pdMS_TO_TICKS(100)

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(int v)     { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days since 1970-01-01 for a calendar date (proleptic Gregorian).
 * newlib has no timegm(), so convert UTC fields to time_t ourselves. */
static long long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

bool rtc_read_utc(time_t *out)
{
    uint8_t reg = RTC_REG_SECONDS;
    uint8_t r[7];
    esp_err_t err = i2c_master_write_read_device(RTC_I2C_PORT, RTC_ADDR, &reg, 1,
                                                 r, sizeof(r), RTC_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(err));
        return false;
    }
    if (r[0] & RTC_OS_FLAG) {
        ESP_LOGW(TAG, "Clock chip lost power; its time is not valid");
        return false;
    }

    int sec  = bcd2bin(r[0] & 0x7F);
    int min  = bcd2bin(r[1] & 0x7F);
    int hour = bcd2bin(r[2] & 0x3F);
    int day  = bcd2bin(r[3] & 0x3F);
    int mon  = bcd2bin(r[5] & 0x1F);
    int year = 2000 + bcd2bin(r[6]);

    if (year < 2025 || mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59) {
        ESP_LOGW(TAG, "Clock chip holds an invalid date (%04d-%02d-%02d)",
                 year, mon, day);
        return false;
    }

    *out = (time_t)(days_from_civil(year, mon, day) * 86400LL +
                    hour * 3600LL + min * 60LL + sec);
    return true;
}

bool rtc_write_utc(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 2000 || tm.tm_year + 1900 > 2099) {
        return false;
    }

    uint8_t buf[8] = {
        RTC_REG_SECONDS,
        bin2bcd(tm.tm_sec),          /* writing seconds also clears OS */
        bin2bcd(tm.tm_min),
        bin2bcd(tm.tm_hour),
        bin2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,         /* 0 = Sunday, same as the chip */
        bin2bcd(tm.tm_mon + 1),
        bin2bcd(tm.tm_year + 1900 - 2000),
    };
    esp_err_t err = i2c_master_write_to_device(RTC_I2C_PORT, RTC_ADDR, buf,
                                               sizeof(buf), RTC_TIMEOUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Write failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
