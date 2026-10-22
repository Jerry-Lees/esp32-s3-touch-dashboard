#pragma once
/*
 * Server monitor engine: checks the hosts listed in SD_DIR/monitors.json
 * (format "dashboard-monitor", version 1; see monitor/monitors.example.json
 * and the README) every `interval` seconds. The UI is app_monitor.c.
 *
 * Checks, written as strings in the file:
 *   "ping"                    ICMP echo
 *   "tcp <port> [label]"      the port accepts a connection
 *   "http <url> [label]"      an HTTP(S) GET answers with any status < 500
 *                             (https needs a certificate the display trusts;
 *                             use "tcp 443" for self-signed ones)
 * A check counts as down after two failures in a row (one blip doesn't
 * flash red). The file is re-read when it changes on the card.
 *
 * Threads: a coordinator task and MON_WORKERS checker tasks (PSRAM stacks,
 * no flash writes). Results live under a mutex; the LVGL side reads them
 * with monitor_lock()/monitor_unlock() and the accessors below.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MON_GROUPS   12
#define MON_HOSTS    96
#define MON_CHECKS   4
#define MON_NAME     32

typedef enum { MON_PING, MON_TCP, MON_HTTP } mon_kind_t;
typedef enum { MON_UNKNOWN, MON_UP, MON_DOWN } mon_state_t;

typedef struct {
    uint8_t     kind;           /* mon_kind_t */
    uint16_t    port;           /* tcp */
    char        label[20];
    char        url[128];       /* http */
    /* status */
    uint8_t     state;          /* mon_state_t */
    uint8_t     fails;          /* failures in a row */
    int         ms;             /* last answer time, -1 = none */
    uint32_t    since;          /* when the state last changed: monitor_now() (0 = unknown) */
    char        err[32];        /* why the last check failed */
} mon_check_t;

typedef struct {
    uint8_t     group;
    char        name[MON_NAME];
    char        addr[64];
    char        what[64];
    uint8_t     ncheck;
    mon_check_t c[MON_CHECKS];
} mon_host_t;

typedef struct {
    int         interval;       /* seconds */
    int         ngroups;
    char        group[MON_GROUPS][24];
    int         nhosts;
    mon_host_t  h[MON_HOSTS];
} mon_cfg_t;

/* From launcher_start(): starts the tasks (they wait for the SD card). */
void monitor_init(void);

void monitor_lock(void);
void monitor_unlock(void);
/* The list and results (PSRAM). Only between lock and unlock. */
const mon_cfg_t *monitor_cfg(void);

/* History: the last 24 hours in 5-minute slots of the real clock, per host
 * (PSRAM; saved to SD_DIR/monitor_history.bin every 15 minutes and loaded at
 * start-up; kept across list changes for hosts with the same name and
 * address). up/down have bit k set if check k passed/failed in that slot;
 * both 0 = no data. ms = the slowest answer in the slot + 1 (ping preferred;
 * 0 = no answer). Not recorded while the clock isn't set. */
#define MON_HIST_N    288
#define MON_HIST_SEC  300
typedef struct {
    uint8_t     up, down;
    uint16_t    ms;
} mon_hist_t;

/* Copies host's history, oldest slot first, into out[MON_HIST_N]. Slots
 * between two rounds (interval longer than a slot) repeat the last result.
 * Under the lock. */
void monitor_history(int host, mon_hist_t *out);

/* Host state from its checks: UNKNOWN, UP (all up), DOWN (all down);
 * partial = some down. Under the lock. */
mon_state_t monitor_host_state(const mon_host_t *h, bool *partial);

/* +1 whenever results or the list change: the UI redraws. */
uint32_t monitor_version(void);
/* Why there's nothing to show ("" if there is a list). */
const char *monitor_problem(void);
/* Times are seconds since start-up (always right, unlike the clock, which
 * may not be set yet); monitor_now() is the current one. */
uint32_t monitor_now(void);
/* When the last round of checks finished (0 = none yet). */
uint32_t monitor_last_round(void);

/* Check now / re-read the file now. Any task. */
void monitor_kick(void);
void monitor_reload(void);

/* LVGL task (SD card only, no flash): writes cfg as the new monitors.json
 * (via monitors.json.tmp; the old file is kept as monitors.json.bak) and
 * reloads it. Only the list is written (names, addresses, checks, interval),
 * no results. False with a reason if the card couldn't be written. */
bool monitor_save(const mon_cfg_t *cfg, char *why, size_t wl);
/* True from monitor_save() until the coordinator has read the new file
 * (it only swaps lists between rounds, so this can take a few seconds). */
bool monitor_save_pending(void);

/* LVGL side (launcher's notice timer): the next "went down" / "back up"
 * message, if pop-ups are on. */
bool monitor_take_alert(char *msg, size_t len);

/* Options (settings u8): ticker item "mon_tk" (default on), pop-ups
 * "mon_pop" (default on), problems first "mon_sort" (default on). */
bool monitor_opt(const char *key);
void monitor_set_opt(const char *key, bool on);

/* Info ticker source: monitor_ticker_items() is declared in app_framework.h. */
