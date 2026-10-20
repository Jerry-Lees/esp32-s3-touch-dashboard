#pragma once
/*
 * Timers, stopwatch and alarms: the part that keeps running while the
 * Timers app (app_timers.c) is closed. When a timer ends or an alarm is
 * due, the screen wakes and a flashing full-screen alert shows until it's
 * dismissed (the board has no speaker). LVGL task only.
 *
 * Timers and the stopwatch count with the system timer, so they don't
 * care whether the clock is set, but they're lost on a restart. Alarms
 * use the real time (Wi-Fi or the RTC) and are saved in settings (key
 * "alarms"), so they survive restarts and are part of settings backups.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Loads the alarms and starts the background check. From launcher_start(). */
void clock_tools_start(void);

/* True while an alert is on screen (the screen timeout waits). */
bool clock_tools_alerting(void);

/* Short text for the home screen's top bar: the soonest running timer, or
 * the next alarm. False if there's nothing to show. */
bool clock_tools_status(char *out, size_t len);

/* "1:05:09" or "5:09" (seconds); "0:05.27" with hundredths if cs. */
void clock_tools_fmt(char *out, size_t len, int64_t ms, bool cs);

/* ---------- timers ---------- */

#define CT_TIMERS 4

typedef struct {
    bool    used, running;
    int64_t left_ms, total_ms;
} ct_timer_info_t;

int  ct_timer_start(int64_t ms);          /* new running timer: index, or -1 if all are used */
void ct_timer_pause(int i, bool pause);
void ct_timer_restart(int i);             /* back to the full time, running */
void ct_timer_remove(int i);
ct_timer_info_t ct_timer_info(int i);

/* ---------- stopwatch ---------- */

#define CT_LAPS 99

void    ct_sw_start(void);
void    ct_sw_stop(void);
void    ct_sw_reset(void);
bool    ct_sw_lap(void);                  /* false when the lap list is full */
bool    ct_sw_running(void);
int64_t ct_sw_elapsed_ms(void);
int     ct_sw_laps(const int64_t **laps); /* lap times (total at each lap), oldest first */

/* ---------- alarms ---------- */

#define CT_ALARMS 6

typedef struct {
    uint8_t hour, min;
    uint8_t days;                         /* bit 0 = Sunday .. bit 6 = Saturday; 0 = once */
    bool    on;
} ct_alarm_t;

int        ct_alarm_count(void);
ct_alarm_t ct_alarm_get(int i);
void       ct_alarm_set(int i, ct_alarm_t a);      /* saves */
int        ct_alarm_add(ct_alarm_t a);              /* saves; index or -1 when full */
void       ct_alarm_remove(int i);                  /* saves */
/* "Weekdays", "Every day", "Once", "Mon Wed Fri" ... */
void       ct_alarm_days_text(uint8_t days, char *out, size_t len);
/* "7:30 AM" or "07:30", per the 12/24-hour setting. */
void       ct_alarm_time_text(int hour, int min, char *out, size_t len);
