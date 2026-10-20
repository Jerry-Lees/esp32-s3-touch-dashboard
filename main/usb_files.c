/*
 * SD card file transfer over the USB cable (PC side: tools/sdcard.py).
 *
 * Line protocol on the USB-Serial/JTAG console, mixed with the normal log
 * output (the PC ignores lines that don't start with "@@"). Every line we
 * send is one printf, so log lines can't split it.
 *
 *   PC -> display                         display -> PC
 *   @@sd hello                            @@ok hello 1 <card 0/1>
 *   @@sd ls <path>                        @@e <D|F> <size> <date> <name>  ...  @@ok ls <n>
 *   @@sd get <path>                       @@ok get <size>, then per chunk
 *                                         @@d <seq> <crc> <base64>, PC answers
 *   @@sd ack <seq> / @@sd nak <seq>       ... and finally @@end <crc of file>
 *   @@sd put <size> <crc> <overwrite> <path>      @@ok put
 *   @@sd d <seq> <crc> <base64>  (each)   @@ack <seq> / @@nak <seq>, then @@ok done
 *   @@sd rm <path> / @@sd mkdir <path>    @@ok rm / @@ok mkdir
 *   errors: @@err <message>
 *
 * Paths and names are percent-encoded (spaces, %, non-ASCII) and relative
 * to the dashboard folder; nothing outside it can be reached. One chunk is
 * in flight at a time (stop and wait), so the driver's RX buffer never
 * overflows and a lost or damaged line is simply sent again. Files arrive
 * as <name>.part and are renamed only after the whole-file CRC matches.
 * wifi*.json files are never sent to the PC.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"
#include "mbedtls/base64.h"

#include "sd_card.h"
#include "usb_files.h"

static const char *TAG = "usb_files";

#define LINE_MAX     4096
#define CHUNK        2048              /* file bytes per data line (2732 in base64) */
#define REL_MAX      200
#define PATH_MAX_LEN 320
#define TIMEOUT_MS   10000             /* give up on a silent PC */
#define RETRIES      5

static char    *s_line;                /* LINE_MAX + 1 */
static size_t   s_len;
static uint8_t  s_rx[512];             /* bytes read but not used yet */
static size_t   s_rx_len, s_rx_pos;
static bool     s_overflow;
static uint8_t *s_chunk;               /* CHUNK */
static char    *s_b64;                 /* base64 of a chunk + 1 */

/* ---------- CRC-32 (same as zlib / Python's zlib.crc32) ---------- */

static uint32_t s_crc_tab[256];

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
        s_crc_tab[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) crc = s_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---------- lines in and out ---------- */

static char *s_out;                    /* LINE_MAX + 1 */

/* Writes one line straight to the driver (the VFS path goes a character at
 * a time), holding the stdout lock so a log line can't land in the middle. */
static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_out, LINE_MAX + 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > LINE_MAX) n = LINE_MAX;
    flockfile(stdout);
    fflush(stdout);
    for (int sent = 0; sent < n;) {
        int k = usb_serial_jtag_write_bytes(s_out + sent, n - sent, pdMS_TO_TICKS(1000));
        if (k <= 0) break;                 /* nobody reading: drop it, like the log does */
        sent += k;
    }
    funlockfile(stdout);
}

/* Next complete line (without CR/LF) into s_line, or NULL after timeout_ms
 * (-1 = wait forever). Over-long lines are dropped. */
static const char *read_line(int timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (s_rx_pos == s_rx_len) {
            TickType_t wait = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
            if (timeout_ms >= 0) {
                TickType_t used = xTaskGetTickCount() - start;
                if (used >= wait) return NULL;
                wait -= used;
            }
            int n = usb_serial_jtag_read_bytes(s_rx, sizeof(s_rx), wait);
            if (n <= 0) {
                if (timeout_ms >= 0) return NULL;
                continue;
            }
            s_rx_len = n;
            s_rx_pos = 0;
        }
        uint8_t c = s_rx[s_rx_pos++];
        if (c == '\r') continue;
        if (c == '\n') {
            bool bad = s_overflow;
            s_line[s_len] = 0;
            s_len = 0;
            s_overflow = false;
            if (!bad) return s_line;
            continue;
        }
        if (s_len < LINE_MAX) s_line[s_len++] = (char)c;
        else s_overflow = true;
    }
}

/* A new command in the middle of a transfer means the PC side was restarted:
 * the transfer is dropped and the command runs next (it stays in s_line). */
static bool s_pending;

static bool new_command(const char *l)
{
    if (strncmp(l, "@@sd ", 5) != 0) return false;
    l += 5;
    s_pending = strncmp(l, "d ", 2) && strncmp(l, "ack", 3) && strncmp(l, "nak", 3) &&
                strncmp(l, "abort", 5);
    return s_pending;
}

/* ---------- paths ---------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* Percent-decodes `in` (a path relative to the dashboard folder) and builds
 * the full path. Rejects anything that could leave the folder. */
static bool make_path(const char *in, char *full, size_t len, char *rel_out)
{
    char rel[REL_MAX];
    size_t k = 0;
    for (const char *p = in; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int h = hexval(p[1]), l = h < 0 ? -1 : hexval(p[2]);
            if (l < 0) return false;
            c = h << 4 | l;
            p += 2;
        }
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':') return false;
        if (k + 1 >= sizeof(rel)) return false;
        rel[k++] = (char)c;
    }
    while (k && rel[k - 1] == '/') k--;                /* "pictures/" = "pictures" */
    rel[k] = 0;
    if (rel[0] == '/') return false;
    /* No empty, "." or ".." parts. */
    for (const char *p = rel; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == 0 || (n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.')) return false;
        p += n + (e ? 1 : 0);
    }
    if (rel_out) strcpy(rel_out, rel);
    return snprintf(full, len, "%s%s%s", SD_DIR, rel[0] ? "/" : "", rel) < (int)len;
}

static void put_pct(char *o, size_t len, const char *name)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && k + 4 < len; p++) {
        if (*p > 0x20 && *p < 0x7F && *p != '%') o[k++] = *p;
        else k += snprintf(o + k, len - k, "%%%02X", *p);
    }
    o[k] = 0;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* The dashboard's own folders aren't removable. */
static bool is_protected(const char *rel)
{
    static const char *const keep[] = { "", "import", "pictures", "screenshots", "backup", "apps" };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        if (!strcasecmp(rel, keep[i])) return true;
    }
    return false;
}

/* ---------- commands ---------- */

static void cmd_ls(const char *arg)
{
    char full[PATH_MAX_LEN], fp[PATH_MAX_LEN];
    if (!make_path(arg, full, sizeof(full), NULL) || !sd_card_fatfs_path(full, fp, sizeof(fp))) {
        out("@@err bad path\n");
        return;
    }
    FF_DIR d;
    if (f_opendir(&d, fp) != FR_OK) {
        out("@@err not a folder\n");
        return;
    }
    static FILINFO fi;
    char name[3 * 256 + 1];
    int n = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        put_pct(name, sizeof(name), fi.fname);
        out("@@e %c %llu %04d-%02d-%02dT%02d:%02d %s\n", fi.fattrib & AM_DIR ? 'D' : 'F',
            (unsigned long long)fi.fsize, 1980 + (fi.fdate >> 9), (fi.fdate >> 5) & 15,
            fi.fdate & 31, fi.ftime >> 11, (fi.ftime >> 5) & 63, name);
        n++;
    }
    f_closedir(&d);
    out("@@ok ls %d\n", n);
}

static void cmd_get(const char *arg)
{
    char full[PATH_MAX_LEN];
    if (!make_path(arg, full, sizeof(full), NULL)) {
        out("@@err bad path\n");
        return;
    }
    if (sd_card_is_secret_name(base_name(full))) {
        out("@@err this file may hold Wi-Fi passwords, so it's never sent\n");
        return;
    }
    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
        out("@@err no such file\n");
        return;
    }
    FILE *f = fopen(full, "rb");
    if (!f) {
        out("@@err couldn't open it\n");
        return;
    }
    out("@@ok get %lld\n", (long long)st.st_size);
    uint32_t total = 0, seq = 0;
    for (;;) {
        size_t n = fread(s_chunk, 1, CHUNK, f);
        if (n == 0) break;
        total = crc32_update(total, s_chunk, n);
        uint32_t c = crc32_update(0, s_chunk, n);
        size_t bl = 0;
        mbedtls_base64_encode((unsigned char *)s_b64, 4 * ((CHUNK + 2) / 3) + 1, &bl, s_chunk, n);
        bool acked = false;
        for (int tries = 0; tries < RETRIES && !acked; tries++) {
            out("@@d %lu %08lx %s\n", (unsigned long)seq, (unsigned long)c, s_b64);
            for (;;) {
                const char *l = read_line(TIMEOUT_MS / 2);
                if (!l) break;                                   /* resend */
                unsigned long s;
                if (sscanf(l, "@@sd ack %lu", &s) == 1 && s == seq) { acked = true; break; }
                if (sscanf(l, "@@sd nak %lu", &s) == 1 && s == seq) break;
                if (!strncmp(l, "@@sd abort", 10) || new_command(l)) { fclose(f); return; }
            }
        }
        if (!acked) {
            fclose(f);
            ESP_LOGW(TAG, "get: the PC stopped answering");
            return;
        }
        seq++;
    }
    fclose(f);
    out("@@end %08lx\n", (unsigned long)total);
}

static void cmd_put(const char *args)
{
    unsigned long long size;
    unsigned long want_crc;
    int overwrite, pos = 0;
    char full[PATH_MAX_LEN], part[PATH_MAX_LEN + 8], rel[REL_MAX];
    if (sscanf(args, "%llu %lx %d %n", &size, &want_crc, &overwrite, &pos) < 3 || !pos ||
        !make_path(args + pos, full, sizeof(full), rel) || !rel[0]) {
        out("@@err bad request\n");
        return;
    }
    /* Names the dashboard can type and show: printable ASCII only. */
    for (const char *p = rel; *p; p++) {
        if ((unsigned char)*p > 0x7E) {
            out("@@err use plain ASCII letters in file names\n");
            return;
        }
    }
    struct stat st;
    if (stat(full, &st) == 0 && (S_ISDIR(st.st_mode) || !overwrite)) {
        out("@@err %s\n", S_ISDIR(st.st_mode) ? "a folder has that name" : "already there (use --force to replace it)");
        return;
    }
    snprintf(part, sizeof(part), "%s.part", full);
    FILE *f = fopen(part, "wb");
    if (!f) {
        out("@@err couldn't create it (does the folder exist?)\n");
        return;
    }
    out("@@ok put\n");
    unsigned long long got = 0;
    uint32_t total = 0, seq = 0;
    bool ok = true;
    while (got < size) {
        const char *l = read_line(TIMEOUT_MS);
        if (!l || !strncmp(l, "@@sd abort", 10) || new_command(l)) {
            ok = false;
            ESP_LOGW(TAG, "put: %s", l ? "cancelled" : "the PC stopped sending");
            break;
        }
        unsigned long s, c;
        int p2 = 0;
        if (sscanf(l, "@@sd d %lu %lx %n", &s, &c, &p2) < 2 || !p2) continue;   /* not ours */
        size_t n = 0;
        const char *b = l + p2;
        bool good = mbedtls_base64_decode(s_chunk, CHUNK, &n, (const unsigned char *)b, strlen(b)) == 0 &&
                    crc32_update(0, s_chunk, n) == c && n > 0 && got + n <= size;
        if (good && s == seq) {
            if (fwrite(s_chunk, 1, n, f) != n) {
                out("@@err the card is full or failing\n");
                ok = false;
                break;
            }
            total = crc32_update(total, s_chunk, n);
            got += n;
            out("@@ack %lu\n", s);
            seq++;
        } else if (s + 1 == seq) {
            out("@@ack %lu\n", s);                      /* our ack was lost: already have it */
        } else {
            out("@@nak %lu\n", s);
        }
    }
    if (ok && total != want_crc) {
        out("@@err the file arrived damaged (CRC mismatch)\n");
        ok = false;
    }
    if (ok) {
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    }
    fclose(f);
    if (ok && overwrite) unlink(full);
    if (ok && rename(part, full) != 0) ok = false;
    if (!ok) {
        unlink(part);
        if (!s_pending) out("@@err not saved\n");    /* else the PC has moved on */
        return;
    }
    out("@@ok done\n");
    sd_card_notice("Received %s over USB", rel);
}

static void cmd_rm(const char *arg)
{
    char full[PATH_MAX_LEN], rel[REL_MAX] = "";
    struct stat st;
    if (!make_path(arg, full, sizeof(full), rel) || is_protected(rel)) {
        out("@@err %s\n", rel[0] ? "the dashboard uses this folder" : "bad path");
        return;
    }
    if (stat(full, &st) != 0) {
        out("@@err no such file\n");
        return;
    }
    if (S_ISDIR(st.st_mode) ? rmdir(full) != 0 : unlink(full) != 0) {
        out("@@err %s\n", S_ISDIR(st.st_mode) ? "folder isn't empty" : "couldn't delete it");
        return;
    }
    out("@@ok rm\n");
    sd_card_notice("Deleted %s over USB", rel);
}

static void cmd_mkdir(const char *arg)
{
    char full[PATH_MAX_LEN], rel[REL_MAX];
    if (!make_path(arg, full, sizeof(full), rel) || !rel[0]) {
        out("@@err bad path\n");
        return;
    }
    if (mkdir(full, 0775) != 0) {
        out("@@err couldn't make it (already there?)\n");
        return;
    }
    out("@@ok mkdir\n");
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        const char *l = s_pending ? s_line : read_line(-1);
        s_pending = false;
        if (strncmp(l, "@@sd ", 5) != 0) continue;
        const char *c = l + 5;
        bool card = sd_card_mounted();
        if (!strncmp(c, "hello", 5)) {
            out("@@ok hello 1 %d\n", card);
        } else if (!card) {
            out("@@err no SD card in the display\n");
        } else if (!strncmp(c, "ls ", 3) || !strcmp(c, "ls")) {
            cmd_ls(c[2] ? c + 3 : "");
        } else if (!strncmp(c, "get ", 4)) {
            cmd_get(c + 4);
        } else if (!strncmp(c, "put ", 4)) {
            cmd_put(c + 4);
        } else if (!strncmp(c, "rm ", 3)) {
            cmd_rm(c + 3);
        } else if (!strncmp(c, "mkdir ", 6)) {
            cmd_mkdir(c + 6);
        } else if (!strncmp(c, "ack", 3) || !strncmp(c, "nak", 3) || !strncmp(c, "d ", 2) ||
                   !strncmp(c, "abort", 5)) {
            /* left over from a transfer that already ended */
        } else {
            out("@@err unknown command\n");
        }
    }
}

void usb_files_start(void)
{
    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = 8192, .tx_buffer_size = 4096 };
    s_line = heap_caps_malloc(LINE_MAX + 1, MALLOC_CAP_SPIRAM);
    s_out = heap_caps_malloc(LINE_MAX + 1, MALLOC_CAP_SPIRAM);
    s_chunk = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    s_b64 = heap_caps_malloc(4 * ((CHUNK + 2) / 3) + 1, MALLOC_CAP_SPIRAM);
    if (!s_line || !s_out || !s_chunk || !s_b64 || usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "not started");
        return;
    }
    usb_serial_jtag_vfs_use_driver();      /* logs and printf now go through the driver too */
    crc_init();
    xTaskCreate(task, "usb_files", 8192, NULL, 2, NULL);
}
