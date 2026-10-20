/*
 * Files: a file manager for the SD card. Browse folders (sorted by name,
 * date or size; 50 per page), see sizes, dates and free space, view
 * pictures (JPEG/PNG/GIF/BMP, with previous/next) and text files (or any
 * file as hex), rename, delete (folders with everything in them, after
 * asking) and make folders.
 *
 * All card access runs in a worker task, one job at a time; the LVGL side
 * starts a job, blocks taps (spinner) and polls for the end. A generation
 * number drops a result that finishes after the app was closed.
 *
 * Safety: the dashboard's own folders can't be renamed or deleted, and
 * files named wifi*.json* (which may hold Wi-Fi passwords) are never shown.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "lvgl.h"
#include "app_framework.h"
#include "launcher.h"
#include "photo_frame.h"
#include "sd_card.h"
#include "settings.h"

#define PATH_LEN     320
#define NAME_LEN     256
#define MAX_ENTRIES  1000
#define PAGE         50
#define TEXT_MAX     (32 * 1024)          /* text shown from the start of a file */
#define HEX_MAX      4096                 /* bytes shown as hex */
#define TEXT_BUF     (40 * 1024)
#define VIEW_W       800
#define VIEW_H       416
#define BAR_H        60
#define ROW_H        52

#define C_BTN        0x2A333C
#define C_RED        0x8E2430
#define C_DIR        0xFFC94D
#define C_PIC        0xF06292
#define C_TXT        0x64B5F6

typedef struct {
    char     name[NAME_LEN];
    uint64_t size;
    uint16_t date, time;           /* FAT format, local time */
    bool     dir;
} entry_t;

/* ======================= worker ======================= */

typedef enum { J_LIST, J_IMAGE, J_TEXT, J_DELETE, J_RENAME, J_MKDIR } jkind_t;

static struct {
    jkind_t  kind;
    int      gen;
    char     path[PATH_LEN], path2[PATH_LEN];   /* VFS paths ("/sdcard/...") */
    bool     hex;                               /* J_TEXT: force hex */
    /* results */
    bool     ok;
    char     err[96];
    int      count;                             /* J_LIST: entries; J_DELETE: files removed */
    bool     truncated;                         /* J_LIST: too many; J_TEXT: longer file */
    bool     is_hex;                            /* J_TEXT: shown as hex */
    uint64_t total, free_b;                     /* J_LIST: card size */
} J;

static bool         s_busy;                     /* __atomic */
static TaskHandle_t s_task;
static entry_t     *s_ent, *s_ent_new;          /* shown list / list being read (swapped) */
static uint16_t    *s_img;                      /* VIEW_W * VIEW_H RGB565 */
static char        *s_text;                     /* TEXT_BUF */
static int          s_gen;

static void set_err(const char *msg) { strlcpy(J.err, msg, sizeof(J.err)); }

static bool fpath(const char *vfs, char *out)
{
    if (!sd_card_fatfs_path(vfs, out, PATH_LEN)) {
        set_err("The SD card isn't available");
        return false;
    }
    return true;
}

static int cmp_name(const void *a, const void *b);

static void job_list(void)
{
    char p[PATH_LEN];
    if (!fpath(J.path, p)) return;
    FF_DIR d;
    if (f_opendir(&d, p) != FR_OK) {
        set_err("Couldn't open the folder");
        return;
    }
    static FILINFO fi;                          /* worker only; big with long names */
    int n = 0;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (n >= MAX_ENTRIES) {
            J.truncated = true;
            break;
        }
        if (strlen(fi.fname) >= NAME_LEN) continue;
        entry_t *e = &s_ent_new[n++];
        strcpy(e->name, fi.fname);
        e->size = fi.fsize;
        e->date = fi.fdate;
        e->time = fi.ftime;
        e->dir = fi.fattrib & AM_DIR;
    }
    f_closedir(&d);
    J.count = n;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &J.total, &J.free_b) != ESP_OK) J.total = 0;
    J.ok = true;
}

static bool is_text_byte(uint8_t c) { return c >= 0x20 || c == '\n' || c == '\r' || c == '\t'; }

static void job_text(void)
{
    FILE *f = fopen(J.path, "rb");
    if (!f) {
        set_err("Couldn't open the file");
        return;
    }
    uint8_t *raw = heap_caps_malloc(TEXT_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!raw) {
        fclose(f);
        set_err("Out of memory");
        return;
    }
    size_t n = fread(raw, 1, TEXT_MAX + 1, f);
    fclose(f);
    J.truncated = n > TEXT_MAX;
    if (n > TEXT_MAX) n = TEXT_MAX;

    int odd = 0;                                /* binary? look at the start */
    for (size_t i = 0; i < n && i < 1024; i++) {
        if (raw[i] == 0) odd += 100;
        else if (!is_text_byte(raw[i])) odd++;
    }
    J.is_hex = J.hex || odd > 8;
    char *o = s_text;
    size_t cap = TEXT_BUF - 1, k = 0;
    if (J.is_hex) {
        if (n > HEX_MAX) {
            n = HEX_MAX;
            J.truncated = true;
        }
        for (size_t i = 0; i < n && k + 80 < cap; i += 16) {
            k += snprintf(o + k, cap - k, "%06X  ", (unsigned)i);
            for (size_t j = 0; j < 16; j++) {
                if (i + j < n) k += snprintf(o + k, cap - k, "%02X ", raw[i + j]);
                else k += snprintf(o + k, cap - k, "   ");
            }
            o[k++] = ' ';
            for (size_t j = 0; j < 16 && i + j < n; j++) {
                uint8_t c = raw[i + j];
                o[k++] = c >= 0x20 && c < 0x7F ? c : '.';
            }
            o[k++] = '\n';
        }
    } else {
        for (size_t i = 0; i < n && k + 4 < cap; i++) {
            uint8_t c = raw[i];
            if (c == '\r') {
                if (i + 1 < n && raw[i + 1] == '\n') continue;
                o[k++] = '\n';
            } else if (c == '\t') {
                o[k++] = ' ';
                o[k++] = ' ';
            } else if (c == '\n' || (c >= 0x20 && c < 0x7F)) {
                o[k++] = c;
            } else if (c >= 0xC0 || c < 0x80) {     /* start of a UTF-8 character, or control */
                o[k++] = '?';                       /* the font only has ASCII */
            }                                       /* UTF-8 continuation bytes: dropped */
        }
    }
    o[k] = 0;
    free(raw);
    J.ok = true;
}

/* Deletes everything inside FatFs path p (a folder), then the folder.
 * Returns files removed, or -1. Reads the first entry each time instead of
 * deleting while iterating. */
static int rm_tree(char *p, int depth)
{
    static FILINFO fi;
    int n = 0;
    for (;;) {
        FF_DIR d;
        if (f_opendir(&d, p) != FR_OK) return -1;
        FRESULT r = f_readdir(&d, &fi);
        f_closedir(&d);
        if (r != FR_OK) return -1;
        if (!fi.fname[0]) break;                     /* empty now */
        size_t len = strlen(p);
        if (len + 1 + strlen(fi.fname) >= PATH_LEN) return -1;
        p[len] = '/';
        strcpy(p + len + 1, fi.fname);
        bool dir = fi.fattrib & AM_DIR;
        if (fi.fattrib & AM_RDO) f_chmod(p, 0, AM_RDO);
        int k = dir ? (depth < 16 ? rm_tree(p, depth + 1) : -1)
                    : (f_unlink(p) == FR_OK ? 1 : -1);
        p[len] = 0;
        if (k < 0) return -1;
        n += k;
        if (n % 32 == 0) vTaskDelay(1);
    }
    return f_unlink(p) == FR_OK ? n : -1;
}

static void job_delete(void)
{
    char p[PATH_LEN];
    if (!fpath(J.path, p)) return;
    FILINFO fi;
    if (f_stat(p, &fi) != FR_OK) {
        set_err("It's already gone");
        return;
    }
    if (fi.fattrib & AM_DIR) {
        J.count = rm_tree(p, 0);
        if (J.count < 0) {
            set_err("Couldn't delete everything in the folder");
            return;
        }
    } else {
        if (fi.fattrib & AM_RDO) f_chmod(p, 0, AM_RDO);
        if (f_unlink(p) != FR_OK) {
            set_err("Couldn't delete the file");
            return;
        }
        J.count = 1;
    }
    J.ok = true;
}

static void job_rename_mkdir(void)
{
    char a[PATH_LEN], b[PATH_LEN];
    if (!fpath(J.path2, b)) return;
    FILINFO fi;
    if (f_stat(b, &fi) == FR_OK) {
        set_err("Something with that name is already there");
        return;
    }
    FRESULT r;
    if (J.kind == J_MKDIR) {
        r = f_mkdir(b);
    } else {
        if (!fpath(J.path, a)) return;
        r = f_rename(a, b);
    }
    if (r != FR_OK) {
        set_err(J.kind == J_MKDIR ? "Couldn't make the folder" : "Couldn't rename it");
        return;
    }
    J.ok = true;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        J.ok = false;
        J.err[0] = 0;
        J.count = 0;
        J.truncated = false;
        switch (J.kind) {
        case J_LIST:   job_list(); break;
        case J_TEXT:   job_text(); break;
        case J_DELETE: job_delete(); break;
        case J_RENAME:
        case J_MKDIR:  job_rename_mkdir(); break;
        case J_IMAGE: {
            char e[64];
            J.ok = photo_decode_file(J.path, s_img, VIEW_W, VIEW_H, false, e);
            if (!J.ok) snprintf(J.err, sizeof(J.err), "Can't show this picture: %s", e);
            break;
        }
        }
        __atomic_store_n(&s_busy, false, __ATOMIC_RELEASE);
    }
}

/* ======================= LVGL side ======================= */

typedef enum { SORT_NAME, SORT_DATE, SORT_SIZE } sort_t;

static struct {
    lv_obj_t   *parent, *path_lbl, *info_lbl, *sort_lbl, *list, *block, *spinner;
    lv_obj_t   *menu, *viewer, *v_img, *v_msg, *v_bar, *v_name, *kb_page, *kb_ta, *kb_msg;
    lv_timer_t *timer;
    char        cur[PATH_LEN];         /* folder shown (VFS path) */
    int         count, page;
    bool        truncated;
    uint64_t    total, free_b;
    bool        waiting, want_list;
    uint32_t    busy_since;
    void      (*done)(void);           /* runs when the job ends */
    int         sel;                   /* entry the menu/viewer is about */
    bool        kb_mkdir;              /* keyboard page: new folder (else rename) */
    lv_img_dsc_t dsc;
} U;

/* ---------- helpers ---------- */

static bool ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && !strcasecmp(s + a - b, ext);
}

static bool is_text_name(const char *n)
{
    static const char *const ext[] = { ".txt", ".json", ".csv", ".log", ".md", ".ini", ".cfg",
                                       ".conf", ".xml", ".html", ".htm", ".c", ".h", ".py",
                                       ".js", ".yaml", ".yml", ".sh", ".bat" };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        if (ends_with(n, ext[i])) return true;
    }
    return false;
}

/* The font is ASCII only (names are code page 437 on the card). */
static void ascii(char *out, size_t len, const char *in)
{
    size_t k = 0;
    for (; *in && k + 1 < len; in++) out[k++] = (*in >= 0x20 && *in < 0x7F) ? *in : '?';
    out[k] = 0;
}

static void fmt_size(char *out, size_t len, uint64_t b)
{
    if (b < 1024) snprintf(out, len, "%u B", (unsigned)b);
    else if (b < 1024 * 1024) snprintf(out, len, "%.1f KB", b / 1024.0);
    else if (b < 1024ULL * 1024 * 1024) snprintf(out, len, "%.1f MB", b / 1048576.0);
    else snprintf(out, len, "%.1f GB", b / 1073741824.0);
}

static void fmt_date(char *out, size_t len, uint16_t d, uint16_t t)
{
    if (!d) {
        strlcpy(out, "", len);
        return;
    }
    int hr = t >> 11, mn = (t >> 5) & 63;
    if (settings_get_24h()) {
        snprintf(out, len, "%04d-%02d-%02d %02d:%02d", 1980 + (d >> 9), (d >> 5) & 15, d & 31, hr, mn);
    } else {
        snprintf(out, len, "%04d-%02d-%02d %d:%02d %s", 1980 + (d >> 9), (d >> 5) & 15, d & 31,
                 hr % 12 ? hr % 12 : 12, mn, hr < 12 ? "AM" : "PM");
    }
}

static bool child_path(char *out, const char *name)
{
    return snprintf(out, PATH_LEN, "%s/%s", U.cur, name) < PATH_LEN;
}

/* The dashboard's own folders: recreated at boot, so not deletable here. */
static bool is_protected(const char *path)
{
    static const char *const keep[] = { SD_DIR, SD_DIR "/import", SD_DIR "/pictures",
                                        SD_DIR "/screenshots", SD_DIR "/backup", SD_DIR "/apps" };
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        if (!strcasecmp(path, keep[i])) return true;
    }
    return false;
}

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, int w, int h, uint32_t color,
                        lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

/* Full-size page over the app (viewer, keyboard, menu backdrop). */
static lv_obj_t *mk_page(uint32_t bg, lv_opa_t opa)
{
    lv_obj_t *p = lv_obj_create(U.parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, VIEW_W, VIEW_H);
    lv_obj_set_style_bg_color(p, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(p, opa, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);          /* nothing underneath gets taps */
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

/* ---------- jobs ---------- */

static void run(jkind_t kind, const char *path, const char *path2, void (*done)(void))
{
    if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) return;    /* taps are blocked anyway */
    J.kind = kind;
    J.gen = s_gen;
    strlcpy(J.path, path ? path : "", sizeof(J.path));
    strlcpy(J.path2, path2 ? path2 : "", sizeof(J.path2));
    U.done = done;
    U.waiting = true;
    U.busy_since = lv_tick_get();
    lv_obj_clear_flag(U.block, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.block);
    __atomic_store_n(&s_busy, true, __ATOMIC_RELEASE);
    xTaskNotifyGive(s_task);
}

static void list_done(void);

static void reload(void)
{
    run(J_LIST, U.cur, NULL, list_done);
}

/* ---------- list ---------- */

static sort_t sort_mode(void) { return (sort_t)(settings_get_u8("fm_sort", SORT_NAME) % 3); }

static int cmp_name(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;           /* folders first */
    return strcasecmp(x->name, y->name);
}

static int cmp_date(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    uint32_t tx = (uint32_t)x->date << 16 | x->time, ty = (uint32_t)y->date << 16 | y->time;
    if (tx != ty) return tx > ty ? -1 : 1;                   /* newest first */
    return strcasecmp(x->name, y->name);
}

static int cmp_size(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    if (x->size != y->size) return x->size > y->size ? -1 : 1;   /* biggest first */
    return strcasecmp(x->name, y->name);
}

static void sort_entries(void)
{
    static int (*const cmp[])(const void *, const void *) = { cmp_name, cmp_date, cmp_size };
    qsort(s_ent, U.count, sizeof(entry_t), cmp[sort_mode()]);
}

static void open_entry(int i);
static void menu_open(int i);
static void build_list(void);

static void row_clicked(lv_event_t *e)
{
    open_entry((int)(intptr_t)lv_event_get_user_data(e));
}

static void row_long(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    lv_obj_clear_state(row, LV_STATE_PRESSED);
    lv_indev_wait_release(lv_indev_get_act());               /* no click after it */
    menu_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void more_clicked(lv_event_t *e)
{
    menu_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void page_rebuild(void *arg)
{
    (void)arg;
    if (!U.list) return;                         /* app closed meanwhile */
    build_list();
    lv_obj_scroll_to_y(U.list, 0, LV_ANIM_OFF);
}

static void page_clicked(lv_event_t *e)
{
    U.page += (int)(intptr_t)lv_event_get_user_data(e);
    lv_async_call(page_rebuild, NULL);           /* the button is in the list being rebuilt */
}

static void add_page_btn(const char *text, int delta)
{
    mk_btn(U.list, text, 300, 48, C_BTN, page_clicked, (void *)(intptr_t)delta);
}

static void add_row(int i)
{
    const entry_t *e = &s_ent[i];
    char buf[NAME_LEN];
    lv_obj_t *row = lv_obj_create(U.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x33404C), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(row, row_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    const char *sym = LV_SYMBOL_FILE;
    uint32_t col = APP_COLOR_SUBTEXT;
    if (e->dir) { sym = LV_SYMBOL_DIRECTORY; col = C_DIR; }
    else if (photo_is_picture(e->name)) { sym = LV_SYMBOL_IMAGE; col = C_PIC; }
    else if (is_text_name(e->name)) { col = C_TXT; }
    lv_obj_t *ic = mk_label(row, APP_FONT_MEDIUM, col, sym);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);

    ascii(buf, sizeof(buf), e->name);
    lv_obj_t *nm = mk_label(row, APP_FONT_MEDIUM, APP_COLOR_TEXT, buf);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, 400);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 50, 0);

    if (!e->dir) {
        fmt_size(buf, sizeof(buf), e->size);
        lv_obj_t *sz = mk_label(row, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
        lv_obj_set_width(sz, 80);
        lv_obj_set_style_text_align(sz, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(sz, LV_ALIGN_LEFT_MID, 456, 0);
    }
    fmt_date(buf, sizeof(buf), e->date, e->time);
    lv_obj_t *dt = mk_label(row, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, buf);
    lv_obj_align(dt, LV_ALIGN_LEFT_MID, 552, 0);

    lv_obj_t *more = mk_btn(row, LV_SYMBOL_LIST, 56, 40, C_BTN, more_clicked, (void *)(intptr_t)i);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, -6, 0);
}

static void update_header(void)
{
    char shown[PATH_LEN + 16], tmp[PATH_LEN];
    ascii(tmp, sizeof(tmp), U.cur + strlen(SD_MOUNT_POINT));
    snprintf(shown, sizeof(shown), "SD card%s", tmp);
    lv_label_set_text(U.path_lbl, shown);

    char fr[24], tot[24], info[96];
    fmt_size(fr, sizeof(fr), U.free_b);
    fmt_size(tot, sizeof(tot), U.total);
    if (U.total) {
        snprintf(info, sizeof(info), "%d item%s%s   |   %s free of %s", U.count,
                 U.count == 1 ? "" : "s", U.truncated ? " (first 1000)" : "", fr, tot);
    } else {
        snprintf(info, sizeof(info), "%d item%s", U.count, U.count == 1 ? "" : "s");
    }
    lv_label_set_text(U.info_lbl, info);
    static const char *const names[] = { "Name", "Date", "Size" };
    lv_label_set_text_fmt(U.sort_lbl, LV_SYMBOL_LIST " %s", names[sort_mode()]);
}

static void build_list(void)
{
    lv_obj_clean(U.list);
    update_header();
    if (U.count == 0) {
        lv_obj_t *l = mk_label(U.list, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "This folder is empty.");
        lv_obj_set_style_pad_all(l, 20, 0);
        return;
    }
    int pages = (U.count + PAGE - 1) / PAGE;
    if (U.page >= pages) U.page = pages - 1;
    if (U.page < 0) U.page = 0;
    int a = U.page * PAGE, b = a + PAGE < U.count ? a + PAGE : U.count;
    char t[48];
    if (U.page > 0) {
        snprintf(t, sizeof(t), LV_SYMBOL_UP "  Items %d-%d", a - PAGE + 1, a);
        add_page_btn(t, -1);
    }
    for (int i = a; i < b; i++) add_row(i);
    if (b < U.count) {
        snprintf(t, sizeof(t), LV_SYMBOL_DOWN "  Items %d-%d", b + 1,
                 b + PAGE < U.count ? b + PAGE : U.count);
        add_page_btn(t, +1);
    }
}

static void list_done(void)
{
    if (!J.ok) {
        launcher_toast(J.err, 4000);
        /* The folder may be gone (deleted on a PC): go back to the card's root. */
        if (strcmp(U.cur, SD_MOUNT_POINT) != 0) {
            strlcpy(U.cur, SD_MOUNT_POINT, sizeof(U.cur));
            reload();
        }
        return;
    }
    entry_t *t = s_ent;                       /* the new list becomes the shown one */
    s_ent = s_ent_new;
    s_ent_new = t;
    U.count = J.count;
    U.truncated = J.truncated;
    U.total = J.total;
    U.free_b = J.free_b;
    sort_entries();
    build_list();
}

static void go_to(const char *path)
{
    strlcpy(U.cur, path, sizeof(U.cur));
    U.page = 0;
    reload();
}

static void up_clicked(lv_event_t *e)
{
    (void)e;
    if (strcmp(U.cur, SD_MOUNT_POINT) == 0) return;
    char p[PATH_LEN];
    strlcpy(p, U.cur, sizeof(p));
    char *slash = strrchr(p, '/');
    if (slash && slash != p) *slash = 0;
    if (strlen(p) < strlen(SD_MOUNT_POINT)) strlcpy(p, SD_MOUNT_POINT, sizeof(p));
    go_to(p);
}

static void sort_clicked(lv_event_t *e)
{
    (void)e;
    settings_set_u8("fm_sort", (sort_mode() + 1) % 3);
    sort_entries();
    U.page = 0;
    build_list();
}

static void refresh_clicked(lv_event_t *e) { (void)e; reload(); }

/* ---------- viewer (pictures and text) ---------- */

static void viewer_close(void)
{
    if (U.viewer) lv_obj_del_async(U.viewer);   /* may be inside its own event */
    U.viewer = U.v_img = U.v_msg = U.v_bar = U.v_name = NULL;
}

static void viewer_back(lv_event_t *e) { (void)e; viewer_close(); }

static void delete_ask(int i);
static void viewer_delete(lv_event_t *e) { (void)e; delete_ask(U.sel); }

static void viewer_tapped(lv_event_t *e)
{
    if (lv_event_get_target(e) != U.viewer || !U.v_bar) return;
    if (lv_obj_has_flag(U.v_bar, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(U.v_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.v_bar, LV_OBJ_FLAG_HIDDEN);
}

static void image_done(void)
{
    if (!U.viewer || !U.v_img) return;
    if (!J.ok) {
        lv_label_set_text(U.v_msg, J.err);
        lv_obj_clear_flag(U.v_msg, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(U.v_msg, LV_OBJ_FLAG_HIDDEN);
    lv_img_cache_invalidate_src(&U.dsc);
    lv_img_set_src(U.v_img, &U.dsc);
    lv_obj_clear_flag(U.v_img, LV_OBJ_FLAG_HIDDEN);
}

static void show_image(int i)
{
    char path[PATH_LEN], name[NAME_LEN];
    if (!child_path(path, s_ent[i].name)) return;
    U.sel = i;
    ascii(name, sizeof(name), s_ent[i].name);
    lv_label_set_text(U.v_name, name);
    lv_obj_add_flag(U.v_img, LV_OBJ_FLAG_HIDDEN);          /* the buffer is about to change */
    lv_label_set_text(U.v_msg, "Loading...");
    lv_obj_clear_flag(U.v_msg, LV_OBJ_FLAG_HIDDEN);
    run(J_IMAGE, path, NULL, image_done);
}

static void image_step(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    for (int k = 1; k < U.count; k++) {
        int i = ((U.sel + dir * k) % U.count + U.count) % U.count;
        if (!s_ent[i].dir && photo_is_picture(s_ent[i].name)) {
            show_image(i);
            return;
        }
    }
}

static void viewer_open(bool image)
{
    viewer_close();
    U.viewer = mk_page(image ? 0x000000 : APP_COLOR_BG, LV_OPA_COVER);
    lv_obj_add_event_cb(U.viewer, viewer_tapped, LV_EVENT_CLICKED, NULL);

    if (image) {
        U.v_img = lv_img_create(U.viewer);
        lv_obj_clear_flag(U.v_img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(U.v_img, LV_OBJ_FLAG_HIDDEN);
    }
    U.v_msg = mk_label(U.viewer, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");
    lv_obj_center(U.v_msg);

    U.v_bar = lv_obj_create(U.viewer);
    lv_obj_remove_style_all(U.v_bar);
    lv_obj_set_size(U.v_bar, VIEW_W, BAR_H);
    lv_obj_set_style_bg_color(U.v_bar, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(U.v_bar, LV_OPA_80, 0);
    lv_obj_add_flag(U.v_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(U.v_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mk_btn(U.v_bar, LV_SYMBOL_LEFT "  Back", 110, 46, C_BTN, viewer_back, NULL),
                 LV_ALIGN_LEFT_MID, 8, 0);
    U.v_name = mk_label(U.v_bar, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_long_mode(U.v_name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.v_name, image ? 400 : 540);
    lv_obj_align(U.v_name, LV_ALIGN_LEFT_MID, 130, 0);
    int x = -8;
    lv_obj_t *del = mk_btn(U.v_bar, LV_SYMBOL_TRASH, 60, 46, C_RED, viewer_delete, NULL);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, x, 0);
    if (image) {
        lv_obj_align(mk_btn(U.v_bar, LV_SYMBOL_NEXT, 60, 46, C_BTN, image_step, (void *)1),
                     LV_ALIGN_RIGHT_MID, x - 70, 0);
        lv_obj_align(mk_btn(U.v_bar, LV_SYMBOL_PREV, 60, 46, C_BTN, image_step, (void *)-1),
                     LV_ALIGN_RIGHT_MID, x - 140, 0);
    }
}

static void text_done(void)
{
    if (!U.viewer) return;
    if (!J.ok) {
        lv_label_set_text(U.v_msg, J.err);
        return;
    }
    lv_obj_add_flag(U.v_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *box = lv_obj_create(U.viewer);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, VIEW_W, VIEW_H - BAR_H);
    lv_obj_set_pos(box, 0, BAR_H);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_t *t = mk_label(box, J.is_hex ? &lv_font_unscii_16 : LV_FONT_DEFAULT, 0xDDE3E8,
                           s_text[0] ? s_text : "(empty file)");
    lv_obj_set_width(t, VIEW_W - 24);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    if (J.truncated || J.is_hex) {
        char note[96];
        snprintf(note, sizeof(note), "%s%s", J.is_hex ? "Hex view" : "",
                 J.truncated ? (J.is_hex ? " - first 4 KB" : "Only the first 32 KB is shown") : "");
        lv_obj_t *n = mk_label(box, LV_FONT_DEFAULT, 0xFFB74D, note);
        lv_obj_align_to(n, t, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    }
}

static void show_text(int i, bool hex)
{
    char path[PATH_LEN], name[NAME_LEN];
    if (sd_card_is_secret_name(s_ent[i].name)) {
        launcher_toast("This file may hold Wi-Fi passwords, so it isn't shown", 5000);
        return;
    }
    if (!child_path(path, s_ent[i].name)) return;
    U.sel = i;
    viewer_open(false);
    ascii(name, sizeof(name), s_ent[i].name);
    lv_label_set_text(U.v_name, name);
    lv_label_set_text(U.v_msg, "Loading...");
    J.hex = hex;
    run(J_TEXT, path, NULL, text_done);
}

static void open_entry(int i)
{
    if (i < 0 || i >= U.count) return;
    const entry_t *e = &s_ent[i];
    char path[PATH_LEN];
    if (e->dir) {
        if (child_path(path, e->name)) go_to(path);
        else launcher_toast("That folder's path is too long", 4000);
    } else if (photo_is_picture(e->name)) {
        viewer_open(true);
        show_image(i);
    } else if (is_text_name(e->name)) {
        show_text(i, false);
    } else {
        menu_open(i);                          /* unknown type: offer text / hex */
    }
}

/* ---------- delete ---------- */

static void menu_close(void)
{
    if (U.menu) lv_obj_del_async(U.menu);
    U.menu = NULL;
}

static void delete_done(void)
{
    char msg[64];
    if (!J.ok) launcher_toast(J.err, 5000);
    else if (J.count > 1) {
        snprintf(msg, sizeof(msg), "Deleted %d files", J.count);
        launcher_toast(msg, 3000);
    }
    reload();
}

static void delete_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    menu_close();
    if (!yes) return;
    char path[PATH_LEN];
    if (!child_path(path, s_ent[U.sel].name)) return;
    viewer_close();
    run(J_DELETE, path, NULL, delete_done);
}

/* A centred box over a dim backdrop; returns the box. */
static lv_obj_t *menu_box(void)
{
    menu_close();
    U.menu = mk_page(0x000000, LV_OPA_60);
    lv_obj_t *box = lv_obj_create(U.menu);
    lv_obj_set_width(box, 600);
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(APP_COLOR_PANEL), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3A4652), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_set_style_pad_column(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *menu_text(lv_obj_t *box, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = mk_label(box, font, color, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

static void delete_ask(int i)
{
    char path[PATH_LEN], name[NAME_LEN], q[NAME_LEN + 64];
    if (!child_path(path, s_ent[i].name)) return;
    if (is_protected(path)) {
        launcher_toast("The dashboard uses this folder, so it can't be deleted", 4000);
        return;
    }
    U.sel = i;
    ascii(name, sizeof(name), s_ent[i].name);
    lv_obj_t *box = menu_box();
    snprintf(q, sizeof(q), s_ent[i].dir ? "Delete the folder \"%s\" and everything in it?"
                                        : "Delete \"%s\"?", name);
    menu_text(box, APP_FONT_MEDIUM, APP_COLOR_TEXT, q);
    menu_text(box, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "It's erased from the SD card. This can't be undone.");
    mk_btn(box, LV_SYMBOL_TRASH "  Delete", 200, 56, C_RED, delete_answer, (void *)1);
    mk_btn(box, "Cancel", 200, 56, C_BTN, delete_answer, NULL);
}

/* ---------- rename / new folder (keyboard page) ---------- */

static void kb_close(void)
{
    if (U.kb_page) lv_obj_del_async(U.kb_page);
    U.kb_page = U.kb_ta = U.kb_msg = NULL;
}

static void kb_job_done(void)
{
    if (!J.ok) {
        if (U.kb_msg) lv_label_set_text(U.kb_msg, J.err);   /* keep the page: fix the name */
        return;
    }
    kb_close();
    reload();
}

static const char *name_problem(const char *n)
{
    size_t len = strlen(n);
    if (len == 0) return "Type a name";
    if (!strcmp(n, ".") || !strcmp(n, "..")) return "That name isn't allowed";
    if (n[0] == ' ' || n[len - 1] == ' ' || n[len - 1] == '.')
        return "A name can't start or end with a space, or end with a dot";
    for (const char *p = n; *p; p++) {
        if ((unsigned char)*p < 0x20 || strchr("\\/:*?\"<>|", *p))
            return "A name can't contain \\ / : * ? \" < > |";
    }
    return NULL;
}

static void kb_save(void)
{
    const char *n = lv_textarea_get_text(U.kb_ta);
    const char *bad = name_problem(n);
    if (bad) {
        lv_label_set_text(U.kb_msg, bad);
        return;
    }
    char to[PATH_LEN], from[PATH_LEN];
    if (!child_path(to, n)) {
        lv_label_set_text(U.kb_msg, "That name is too long");
        return;
    }
    if (U.kb_mkdir) {
        run(J_MKDIR, NULL, to, kb_job_done);
    } else {
        if (!child_path(from, s_ent[U.sel].name)) return;
        if (!strcmp(from, to)) {
            kb_close();
            return;
        }
        run(J_RENAME, from, to, kb_job_done);
    }
}

static void kb_save_clicked(lv_event_t *e) { (void)e; kb_save(); }
static void kb_back_clicked(lv_event_t *e) { (void)e; kb_close(); }

static void kb_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) kb_save();
    else kb_close();
}

static void kb_open(bool mkdir, const char *title, const char *initial)
{
    kb_close();
    U.kb_mkdir = mkdir;
    U.kb_page = mk_page(APP_COLOR_BG, LV_OPA_COVER);
    lv_obj_set_pos(mk_label(U.kb_page, APP_FONT_MEDIUM, APP_COLOR_TEXT, title), 16, 18);
    lv_obj_align(mk_btn(U.kb_page, LV_SYMBOL_OK "  Save", 130, 46, 0x1B8F4A, kb_save_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -140, 8);
    lv_obj_align(mk_btn(U.kb_page, LV_SYMBOL_LEFT "  Back", 116, 46, C_BTN, kb_back_clicked, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    U.kb_ta = lv_textarea_create(U.kb_page);
    lv_textarea_set_one_line(U.kb_ta, true);
    lv_textarea_set_max_length(U.kb_ta, 120);
    lv_textarea_set_text(U.kb_ta, initial);
    lv_obj_set_style_text_font(U.kb_ta, APP_FONT_MEDIUM, 0);
    lv_obj_set_size(U.kb_ta, 768, 52);
    lv_obj_set_pos(U.kb_ta, 16, 66);
    lv_obj_add_state(U.kb_ta, LV_STATE_FOCUSED);
    U.kb_msg = mk_label(U.kb_page, LV_FONT_DEFAULT, 0xFFB74D, "");
    lv_obj_set_pos(U.kb_msg, 16, 128);
    lv_obj_t *kb = lv_keyboard_create(U.kb_page);
    lv_obj_set_size(kb, VIEW_W, 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, U.kb_ta);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_event, LV_EVENT_CANCEL, NULL);
}

static void newdir_clicked(lv_event_t *e)
{
    (void)e;
    kb_open(true, "New folder name", "");
}

/* ---------- action menu ---------- */

enum { A_OPEN, A_TEXT, A_HEX, A_RENAME, A_DELETE, A_CANCEL };

static void menu_action(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    int i = U.sel;
    menu_close();
    char path[PATH_LEN], name[NAME_LEN];
    switch (a) {
    case A_OPEN:
        if (s_ent[i].dir || photo_is_picture(s_ent[i].name) || is_text_name(s_ent[i].name)) open_entry(i);
        else show_text(i, false);
        break;
    case A_TEXT: show_text(i, false); break;
    case A_HEX:  show_text(i, true); break;
    case A_RENAME:
        if (!child_path(path, s_ent[i].name)) return;
        if (is_protected(path)) {
            launcher_toast("The dashboard uses this folder, so it can't be renamed", 4000);
            return;
        }
        ascii(name, sizeof(name), s_ent[i].name);
        /* Only prefill names the keyboard could have typed. */
        kb_open(false, "Rename to", strcmp(name, s_ent[i].name) ? "" : name);
        break;
    case A_DELETE: delete_ask(i); break;
    default: break;
    }
}

static void menu_open(int i)
{
    if (i < 0 || i >= U.count) return;
    U.sel = i;
    const entry_t *e = &s_ent[i];
    char name[NAME_LEN], line[160], sz[24], dt[40];
    ascii(name, sizeof(name), e->name);
    lv_obj_t *box = menu_box();
    menu_text(box, APP_FONT_MEDIUM, APP_COLOR_TEXT, name);
    fmt_size(sz, sizeof(sz), e->size);
    fmt_date(dt, sizeof(dt), e->date, e->time);
    const char *kind = e->dir ? "Folder" : photo_is_picture(e->name) ? "Picture"
                                         : is_text_name(e->name) ? "Text file" : "File";
    if (e->dir) snprintf(line, sizeof(line), "%s   |   Modified %s", kind, dt[0] ? dt : "-");
    else snprintf(line, sizeof(line), "%s   |   %s (%llu bytes)   |   Modified %s", kind, sz,
                  (unsigned long long)e->size, dt[0] ? dt : "-");
    menu_text(box, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, line);

    mk_btn(box, e->dir ? LV_SYMBOL_DIRECTORY "  Open" : LV_SYMBOL_EYE_OPEN "  Open",
           170, 56, C_BTN, menu_action, (void *)A_OPEN);
    if (!e->dir) {
        mk_btn(box, "View as text", 170, 56, C_BTN, menu_action, (void *)A_TEXT);
        mk_btn(box, "View as hex", 170, 56, C_BTN, menu_action, (void *)A_HEX);
    }
    mk_btn(box, LV_SYMBOL_EDIT "  Rename", 170, 56, C_BTN, menu_action, (void *)A_RENAME);
    mk_btn(box, LV_SYMBOL_TRASH "  Delete", 170, 56, C_RED, menu_action, (void *)A_DELETE);
    mk_btn(box, "Cancel", 170, 56, C_BTN, menu_action, (void *)A_CANCEL);
}

/* ---------- timer, create, destroy ---------- */

static void tick(lv_timer_t *t)
{
    (void)t;
    bool busy = __atomic_load_n(&s_busy, __ATOMIC_ACQUIRE);
    if (U.want_list && !busy) {                /* a job from the last visit has finished */
        U.want_list = false;
        reload();
        return;
    }
    if (!U.waiting) return;
    if (busy) {
        if (lv_tick_elaps(U.busy_since) > 300) lv_obj_clear_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    U.waiting = false;
    lv_obj_add_flag(U.block, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);
    if (J.gen == s_gen && U.done) U.done();
}

static void files_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.parent = parent;
    s_gen++;
    lv_obj_set_style_bg_color(parent, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!sd_card_mounted()) {
        lv_obj_t *l = mk_label(parent, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT,
                               "No SD card.\nInsert one and restart the display.");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        return;
    }
    if (!s_task) {
        s_ent = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        s_ent_new = heap_caps_calloc(MAX_ENTRIES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
        s_img = heap_caps_malloc(VIEW_W * VIEW_H * 2, MALLOC_CAP_SPIRAM);
        s_text = heap_caps_malloc(TEXT_BUF, MALLOC_CAP_SPIRAM);
        if (!s_ent || !s_ent_new || !s_img || !s_text ||
            xTaskCreate(worker, "files", 8192, NULL, 2, &s_task) != pdPASS) {
            s_task = NULL;
            lv_obj_center(mk_label(parent, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT,
                                   "Not enough memory for the file manager."));
            return;
        }
    }
    U.dsc = (lv_img_dsc_t){ .header.cf = LV_IMG_CF_TRUE_COLOR, .header.w = VIEW_W,
                            .header.h = VIEW_H, .data_size = VIEW_W * VIEW_H * 2,
                            .data = (const uint8_t *)s_img };

    /* Toolbar: Up | path + info | Sort | New folder | Refresh */
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, VIEW_W, BAR_H);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mk_btn(bar, LV_SYMBOL_UP "  Up", 90, 46, C_BTN, up_clicked, NULL),
                 LV_ALIGN_LEFT_MID, 8, 0);
    U.path_lbl = mk_label(bar, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_long_mode(U.path_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.path_lbl, 380);
    lv_obj_set_pos(U.path_lbl, 110, 6);
    U.info_lbl = mk_label(bar, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, "");
    lv_obj_set_pos(U.info_lbl, 110, 34);
    lv_obj_t *sb = mk_btn(bar, "", 104, 46, C_BTN, sort_clicked, NULL);
    U.sort_lbl = lv_obj_get_child(sb, 0);
    lv_obj_align(sb, LV_ALIGN_RIGHT_MID, -196, 0);
    lv_obj_align(mk_btn(bar, LV_SYMBOL_PLUS " Folder", 120, 46, C_BTN, newdir_clicked, NULL),
                 LV_ALIGN_RIGHT_MID, -68, 0);
    lv_obj_align(mk_btn(bar, LV_SYMBOL_REFRESH, 54, 46, C_BTN, refresh_clicked, NULL),
                 LV_ALIGN_RIGHT_MID, -8, 0);

    U.list = lv_obj_create(parent);
    lv_obj_remove_style_all(U.list);
    lv_obj_set_size(U.list, VIEW_W, VIEW_H - BAR_H);
    lv_obj_set_pos(U.list, 0, BAR_H);
    lv_obj_set_style_pad_hor(U.list, 8, 0);
    lv_obj_set_style_pad_bottom(U.list, 8, 0);
    lv_obj_set_style_pad_row(U.list, 6, 0);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(U.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);

    /* Blocks taps while a job runs; the spinner shows if it takes a while. */
    U.block = mk_page(0x000000, LV_OPA_TRANSP);
    lv_obj_add_flag(U.block, LV_OBJ_FLAG_HIDDEN);
    U.spinner = lv_spinner_create(U.block, 1000, 60);
    lv_obj_set_size(U.spinner, 80, 80);
    lv_obj_center(U.spinner);
    lv_obj_add_flag(U.spinner, LV_OBJ_FLAG_HIDDEN);

    strlcpy(U.cur, SD_DIR, sizeof(U.cur));       /* start in the dashboard folder */
    U.timer = lv_timer_create(tick, 100, NULL);
    if (__atomic_load_n(&s_busy, __ATOMIC_ACQUIRE)) U.want_list = true;
    else reload();
}

static void files_destroy(void)
{
    if (U.timer) lv_timer_del(U.timer);
    s_gen++;                                     /* a running job's result is dropped */
    memset(&U, 0, sizeof(U));                    /* the launcher deletes the objects */
}

const app_t files_app = {
    .name      = "Files",
    .symbol    = LV_SYMBOL_DIRECTORY,
    .color_hex = 0x00897B,
    .create    = files_create,
    .destroy   = files_destroy,
};
