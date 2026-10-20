/*
 * DashPad: pages of big buttons that run actions (on the PC through the
 * DashPad helper, web requests, page switches). The engine (layout,
 * storage, actions, pairing) is dashpad.c.
 *
 * - Tap a button to run it; a green or red flash shows how it went (errors
 *   also show as a message).
 * - Edit (bottom bar) shows empty slots; tap a button or slot to edit it.
 *   Holding a button edits it too.
 * - The menu: add / rename / delete pages, export the layout to the SD
 *   card, reset to the example layout, and the PC helper's pairing.
 *
 * Text fields open a full-width keyboard page, so the keyboard never covers
 * the field being typed in.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "lvgl.h"
#include "app_framework.h"
#include "dashpad.h"
#include "game_common.h"
#include "launcher.h"
#include "usb_kbd.h"

#define AREA_W    800
#define AREA_H    416
#define BAR_H     56
#define MARGIN    12
#define GAP       10
#define BTN_W     ((AREA_W - 2 * MARGIN - (DP_COLS - 1) * GAP) / DP_COLS)
#define BTN_H     ((AREA_H - BAR_H - 2 * MARGIN - 2 * GAP) / 3)
#define C_BAR     0x1A2128
#define C_BUTTON  0x2A333C
#define C_ACCENT  0x42A5F5

static const uint32_t COLORS[] = {
    0x37474F, 0x1E3A5F, 0x1565C0, 0x00838F, 0x2E7D32,
    0x9E9D24, 0xE66000, 0xB71C1C, 0xAD1457, 0x5C6BC0,
};
#define N_COLORS ((int)(sizeof(COLORS) / sizeof(COLORS[0])))

static struct {
    lv_obj_t   *root, *grid, *slot[DP_BUTTONS], *page_lbl, *pc_lbl, *edit_btn;
    lv_timer_t *timer;
    int         page;
    bool        edit, long_pressed;
    uint32_t    version;
    uint32_t    flash_until[DP_BUTTONS];
    /* editor */
    lv_obj_t   *ed, *ed_label, *ed_kind, *ed_detail, *ed_syms;
    int         ed_index;
    /* text prompt */
    lv_obj_t   *prompt, *prompt_ta;
    char       *prompt_out;
    size_t      prompt_len;
    void      (*prompt_done)(void);
    /* menu */
    lv_obj_t   *menu;
} U;

static dp_button_t E EXT_RAM_BSS_ATTR;            /* the button being edited */
static const char *s_sym_map[DP_BUTTONS * 4 + 64]; /* symbol picker (10 per row) */
static const char *s_page_map[DP_PAGES * 2 + 1];
static char        s_pc_names[16][32];
static const char *s_pc_map[16 * 2 + 1];

static void rebuild_grid(void);
static void update_keyboard(void);
static void editor_open(int index);
static void editor_refresh_detail(void);

/* ======================= small helpers ======================= */

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *button(lv_obj_t *p, const char *text, int w, int h, uint32_t color,
                        lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    lv_obj_center(label(b, APP_FONT_MEDIUM, APP_COLOR_TEXT, text));
    return b;
}

static lv_obj_t *overlay(void)
{
    lv_obj_t *o = lv_obj_create(U.root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, AREA_W, AREA_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(APP_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);           /* blocks what's underneath */
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static dp_page_t *page(void) { return &dashpad_deck()->p[U.page]; }

static void save(void)
{
    if (!dashpad_save()) launcher_toast("Couldn't save the DashPad layout", 4000);
}

/* ======================= text prompt (full keyboard) ======================= */

static void prompt_close(void)
{
    if (U.prompt) lv_obj_del_async(U.prompt);
    U.prompt = U.prompt_ta = NULL;
}

static void prompt_kb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && U.prompt_out) {
        snprintf(U.prompt_out, U.prompt_len, "%s", lv_textarea_get_text(U.prompt_ta));
        if (U.prompt_done) U.prompt_done();
    }
    prompt_close();
}

static void prompt_ok(lv_event_t *e)
{
    (void)e;
    lv_event_t ev = { .code = LV_EVENT_READY };
    prompt_kb(&ev);
}

static void prompt_cancel(lv_event_t *e)
{
    (void)e;
    prompt_close();
}

/* Asks for text; writes it into out (len bytes) and calls done on OK. */
static void prompt(const char *title, char *out, size_t len, void (*done)(void))
{
    U.prompt = overlay();
    U.prompt_out = out;
    U.prompt_len = len;
    U.prompt_done = done;
    lv_obj_set_pos(label(U.prompt, APP_FONT_LARGE, APP_COLOR_TEXT, title), 16, 14);
    lv_obj_align(button(U.prompt, LV_SYMBOL_OK "  OK", 120, 46, 0x1B8F4A, prompt_ok, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_align(button(U.prompt, "Cancel", 120, 46, C_BUTTON, prompt_cancel, NULL),
                 LV_ALIGN_TOP_RIGHT, -142, 8);
    U.prompt_ta = lv_textarea_create(U.prompt);
    lv_textarea_set_one_line(U.prompt_ta, true);
    lv_textarea_set_max_length(U.prompt_ta, len - 1);
    lv_textarea_set_text(U.prompt_ta, out);
    lv_obj_set_style_text_font(U.prompt_ta, APP_FONT_MEDIUM, 0);
    lv_obj_set_size(U.prompt_ta, 768, 52);
    lv_obj_set_pos(U.prompt_ta, 16, 76);
    lv_obj_add_state(U.prompt_ta, LV_STATE_FOCUSED);
    lv_obj_t *kb = lv_keyboard_create(U.prompt);
    lv_obj_set_size(kb, 800, 260);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, U.prompt_ta);
    lv_obj_add_event_cb(kb, prompt_kb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, prompt_kb, LV_EVENT_CANCEL, NULL);
}

/* ======================= the pad ======================= */

static void show_page(int p)
{
    int n = dashpad_deck()->npages;
    U.page = (p % n + n) % n;
    rebuild_grid();
}

static void slot_event(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        U.long_pressed = false;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        U.long_pressed = true;
        editor_open(i);
    } else if (code == LV_EVENT_CLICKED && !U.long_pressed) {
        const dp_button_t *b = &page()->b[i];
        if (U.edit) {
            editor_open(i);
        } else if (b->used && b->act.kind == DP_PAGE) {
            for (int k = 0; k < dashpad_deck()->npages; k++) {
                if (!strcmp(dashpad_deck()->p[k].name, b->act.text)) show_page(k);
            }
        } else if (b->used && b->act.kind != DP_NONE) {
            dashpad_run(b, U.page * DP_BUTTONS + i);
        }
    }
}

static void rebuild_grid(void)
{
    U.version = dashpad_version();
    update_keyboard();
    if (U.page >= dashpad_deck()->npages) U.page = 0;
    lv_obj_clean(U.grid);
    memset(U.slot, 0, sizeof(U.slot));
    memset(U.flash_until, 0, sizeof(U.flash_until));
    for (int i = 0; i < DP_BUTTONS; i++) {
        const dp_button_t *b = &page()->b[i];
        if (!b->used && !U.edit) continue;
        lv_obj_t *s = U.slot[i] = lv_btn_create(U.grid);
        lv_obj_set_size(s, BTN_W, BTN_H);
        lv_obj_set_pos(s, MARGIN + (i % DP_COLS) * (BTN_W + GAP), MARGIN + (i / DP_COLS) * (BTN_H + GAP));
        lv_obj_set_style_radius(s, 16, 0);
        lv_obj_set_style_shadow_width(s, 0, 0);
        lv_obj_set_style_border_color(s, lv_color_white(), 0);
        lv_obj_set_style_border_opa(s, LV_OPA_50, 0);
        lv_obj_set_style_border_width(s, U.edit ? 2 : 0, 0);
        lv_obj_set_style_transform_width(s, -3, LV_STATE_PRESSED);   /* shrinks when pressed */
        lv_obj_set_style_transform_height(s, -3, LV_STATE_PRESSED);
        lv_obj_add_event_cb(s, slot_event, LV_EVENT_ALL, (void *)(intptr_t)i);
        if (!b->used) {
            lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
            lv_obj_center(label(s, APP_FONT_LARGE, 0x5F6B77, LV_SYMBOL_PLUS));
            continue;
        }
        lv_obj_set_style_bg_color(s, lv_color_hex(b->color), 0);
        lv_obj_t *sym = label(s, APP_FONT_LARGE, APP_COLOR_TEXT, dashpad_symbol(b->symbol));
        lv_obj_align(sym, LV_ALIGN_CENTER, 0, b->label[0] ? -14 : 0);
        if (b->label[0]) {
            lv_obj_t *l = label(s, APP_FONT_MEDIUM, APP_COLOR_TEXT, b->label);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l, BTN_W - 12);
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(l, LV_ALIGN_CENTER, 0, 24);
        }
    }
    lv_label_set_text_fmt(U.page_lbl, "%s  (%d/%d)", page()->name, U.page + 1,
                          dashpad_deck()->npages);
    lv_obj_set_style_bg_color(U.edit_btn, lv_color_hex(U.edit ? 0xF9A825 : C_BUTTON), 0);
}

static void prev_clicked(lv_event_t *e) { (void)e; show_page(U.page - 1); }
static void next_clicked(lv_event_t *e) { (void)e; show_page(U.page + 1); }

static void edit_clicked(lv_event_t *e)
{
    (void)e;
    U.edit = !U.edit;
    rebuild_grid();
}

/* The USB keyboard is on while DashPad is open, if switched on in the menu. */
static void update_keyboard(void)
{
    bool want = dashpad_keyboard_on();
    if (want && !usb_kbd_active()) usb_kbd_start();
    else if (!want && usb_kbd_active()) usb_kbd_stop();
}

static void update_pc(void)
{
    static const struct { const char *text; uint32_t color; } S[] = {
        [DP_PC_UNPAIRED] = { "PC: not paired", 0x9AA5B1 },
        [DP_PC_UNKNOWN]  = { "PC: checking...", 0x9AA5B1 },
        [DP_PC_OK]       = { "PC " LV_SYMBOL_OK, 0x66BB6A },
        [DP_PC_DOWN]     = { "PC: offline", 0xEF5350 },
        [DP_PC_BADKEY]   = { "PC: pair again", 0xEF5350 },
        [DP_PC_NOCLOCK]  = { "PC: no clock yet", 0xFFB74D },
        [DP_PC_OFF]      = { "", 0x9AA5B1 },
    };
    dp_pc_state_t s = dashpad_pc_state();
    const char *kb = !usb_kbd_active() ? "" : usb_kbd_ready() ? "   " LV_SYMBOL_KEYBOARD " " LV_SYMBOL_OK
                                                             : "   " LV_SYMBOL_KEYBOARD " ...";
    lv_label_set_text_fmt(U.pc_lbl, "%s%s", S[s].text, kb);
    lv_obj_set_style_text_color(U.pc_lbl, lv_color_hex(S[s].color), 0);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (dashpad_version() != U.version && !U.ed && !U.menu) rebuild_grid();
    update_pc();
    int id;
    bool ok;
    char msg[96];
    while (dashpad_take_result(&id, &ok, msg, sizeof(msg))) {
        int i = id % DP_BUTTONS;
        if (id / DP_BUTTONS == U.page && U.slot[i]) {
            lv_obj_set_style_border_color(U.slot[i], lv_color_hex(ok ? 0x66BB6A : 0xEF5350), 0);
            lv_obj_set_style_border_opa(U.slot[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(U.slot[i], 5, 0);
            U.flash_until[i] = lv_tick_get() + 700;
        }
        if (!ok) launcher_toast(msg, 5000);
    }
    for (int i = 0; i < DP_BUTTONS; i++) {
        if (U.flash_until[i] && (int32_t)(lv_tick_get() - U.flash_until[i]) >= 0 && U.slot[i]) {
            U.flash_until[i] = 0;
            lv_obj_set_style_border_color(U.slot[i], lv_color_white(), 0);
            lv_obj_set_style_border_opa(U.slot[i], LV_OPA_50, 0);
            lv_obj_set_style_border_width(U.slot[i], U.edit ? 2 : 0, 0);
        }
    }
}

/* ======================= editor ======================= */

static void editor_close(void)
{
    if (U.ed) lv_obj_del_async(U.ed);
    U.ed = U.ed_label = U.ed_kind = U.ed_detail = U.ed_syms = NULL;
}

static void ed_cancel(lv_event_t *e) { (void)e; editor_close(); }

static void ed_save(lv_event_t *e)
{
    (void)e;
    char why[64];
    if ((E.act.kind == DP_PC && !E.act.text[0]) || (E.act.kind == DP_URL &&
        strncmp(E.act.text, "http://", 7) && strncmp(E.act.text, "https://", 8))) {
        launcher_toast(E.act.kind == DP_PC ? "Enter the PC action's name"
                                           : "The URL must start with http:// or https://", 4000);
        return;
    }
    if ((E.act.kind == DP_KEYS && !usb_kbd_parse(E.act.text, why, sizeof(why))) ||
        (E.act.kind == DP_PC && E.act.keys[0] && !usb_kbd_parse(E.act.keys, why, sizeof(why)))) {
        launcher_toast(why, 4000);
        return;
    }
    E.used = true;
    page()->b[U.ed_index] = E;
    save();
    editor_close();
    rebuild_grid();
}

static void ed_delete_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (!yes) return;
    memset(&page()->b[U.ed_index], 0, sizeof(dp_button_t));
    save();
    editor_close();
    rebuild_grid();
}

static void ed_delete(lv_event_t *e)
{
    (void)e;
    lv_obj_t *row = game_dialog_open(U.root, "Delete this button?");
    game_dialog_button(row, LV_SYMBOL_TRASH "  Delete", ed_delete_answer, (void *)1);
    game_dialog_button(row, "Cancel", ed_delete_answer, NULL);
}

static void ed_label_done(void) { lv_label_set_text(U.ed_label, E.label[0] ? E.label : "(no label)"); }
static void ed_label_clicked(lv_event_t *e)
{
    (void)e;
    prompt("Button label", E.label, sizeof(E.label), ed_label_done);
}

static void ed_color(lv_event_t *e)
{
    E.color = COLORS[(int)(intptr_t)lv_event_get_user_data(e)];
    lv_obj_t *sw = lv_event_get_target(e), *row = lv_obj_get_parent(sw);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); i++) {
        lv_obj_set_style_border_width(lv_obj_get_child(row, i), 0, 0);
    }
    lv_obj_set_style_border_width(sw, 3, 0);
}

static void ed_symbol(lv_event_t *e)
{
    uint16_t i = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (i < dashpad_symbol_count()) {
        snprintf(E.symbol, sizeof(E.symbol), "%s", dashpad_symbol_name(i));
    }
}

static const char *s_kind_map[] = { "Nothing", "Run on PC", "\n", "Web request", "Go to page",
                                    "\n", "Keyboard keys", "" };
static const uint8_t KIND_OF[] = { DP_NONE, DP_PC, DP_URL, DP_PAGE, DP_KEYS };
#define N_KINDS 5

static void ed_kind(lv_event_t *e)
{
    uint16_t i = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if (i >= N_KINDS || E.act.kind == KIND_OF[i]) return;
    memset(&E.act, 0, sizeof(E.act));
    E.nsteps = 0;
    E.act.kind = KIND_OF[i];
    editor_refresh_detail();
}

static lv_obj_t *matrix(lv_obj_t *p, const char **map, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *m = lv_btnmatrix_create(p);
    lv_btnmatrix_set_map(m, map);
    lv_btnmatrix_set_btn_ctrl_all(m, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(m, true);
    lv_obj_set_size(m, w, h);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 6, 0);
    lv_obj_set_style_radius(m, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_BUTTON), LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_hex(APP_COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, APP_FONT_MEDIUM, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(m, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return m;
}

/* A tappable "field" showing a value; tapping opens the keyboard page. */
static lv_obj_t *field(lv_obj_t *p, const char *value, int y, lv_event_cb_t cb)
{
    lv_obj_t *f = button(p, "", 368, 46, 0x232D36, cb, NULL);
    lv_obj_set_pos(f, 0, y);
    lv_obj_t *l = lv_obj_get_child(f, 0);
    lv_label_set_text(l, value);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 340);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return f;
}

static void det_text_done(void) { editor_refresh_detail(); }
static void det_pc_clicked(lv_event_t *e)
{
    (void)e;
    prompt("Action name (as in the PC helper's helper.json)", E.act.text, 32, det_text_done);
}
static void det_url_clicked(lv_event_t *e)
{
    (void)e;
    prompt("URL (http:// or https://)", E.act.text, sizeof(E.act.text), det_text_done);
}
static void det_body_clicked(lv_event_t *e)
{
    (void)e;
    prompt("Body sent with POST (optional)", E.act.body, sizeof(E.act.body), det_text_done);
}

static void det_pckeys_clicked(lv_event_t *e)
{
    (void)e;
    prompt("Keys sent instead of the helper (optional), e.g. ctrl+alt+t", E.act.keys,
           sizeof(E.act.keys), det_text_done);
}

static void det_keys_clicked(lv_event_t *e)
{
    (void)e;
    prompt("Keys, e.g. ctrl+alt+t  meta+l  f13  play  volup", E.act.text, 64, det_text_done);
}

static void det_pc_pick(lv_event_t *e)
{
    uint16_t i = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    const char *t = lv_btnmatrix_get_btn_text(lv_event_get_target(e), i);
    if (t) snprintf(E.act.text, sizeof(E.act.text), "%s", t);
    editor_refresh_detail();
}

static void det_method(lv_event_t *e)
{
    E.act.post = lv_btnmatrix_get_selected_btn(lv_event_get_target(e)) == 1;
}

static void det_page(lv_event_t *e)
{
    uint16_t i = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    const char *t = lv_btnmatrix_get_btn_text(lv_event_get_target(e), i);
    if (t) snprintf(E.act.text, sizeof(E.act.text), "%s", t);
}

static void editor_refresh_detail(void)
{
    lv_obj_t *d = U.ed_detail;
    lv_obj_clean(d);
    static const char *method_map[] = { "GET", "POST", "" };
    switch (E.act.kind) {
    case DP_PC: {
        char kt[96];
        field(d, E.act.text[0] ? E.act.text : "(tap to enter the action's name)", 0,
              det_pc_clicked);
        snprintf(kt, sizeof(kt), "Keys: %s", E.act.keys[0] ? E.act.keys
                                                            : "(none; used without the helper)");
        field(d, kt, 52, det_pckeys_clicked);
        int n = dashpad_pc_actions(s_pc_names, 6), k = 0;   /* two rows fit */
        if (n == 0) {
            lv_obj_t *l = label(d, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                                "Pair with the PC helper to pick from its list of actions. "
                                "The keys are sent instead when the helper is off or can't "
                                "be reached and the USB keyboard is on.");
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(l, 368);
            lv_obj_set_pos(l, 0, 108);
            break;
        }
        for (int i = 0; i < n; i++) {
            if (i && i % 3 == 0) s_pc_map[k++] = "\n";
            s_pc_map[k++] = s_pc_names[i];
        }
        s_pc_map[k] = "";
        int rows = (n + 2) / 3;
        lv_obj_t *m = matrix(d, s_pc_map, 368, rows * 42, det_pc_pick);
        lv_obj_set_pos(m, 0, 108);
        for (int i = 0; i < n; i++) {
            if (!strcmp(s_pc_names[i], E.act.text)) lv_btnmatrix_set_btn_ctrl(m, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
        break;
    }
    case DP_URL: {
        field(d, E.act.text[0] ? E.act.text : "(tap to enter the URL)", 0, det_url_clicked);
        lv_obj_t *m = matrix(d, method_map, 368, 42, det_method);
        lv_obj_set_pos(m, 0, 56);
        lv_btnmatrix_set_btn_ctrl(m, E.act.post ? 1 : 0, LV_BTNMATRIX_CTRL_CHECKED);
        field(d, E.act.body[0] ? E.act.body : "(POST body, optional)", 108, det_body_clicked);
        break;
    }
    case DP_KEYS: {
        char why[64];
        field(d, E.act.text[0] ? E.act.text : "(tap to enter the keys)", 0, det_keys_clicked);
        bool ok = !E.act.text[0] || usb_kbd_parse(E.act.text, why, sizeof(why));
        lv_obj_t *l = label(d, LV_FONT_DEFAULT, ok ? APP_COLOR_SUBTEXT : 0xEF5350,
                            ok ? "Sent as a USB keyboard while DashPad is open. Modifiers: ctrl "
                                 "shift alt meta. Keys: a-z 0-9 f1-f24 enter esc tab space "
                                 "up down left right home end pageup pagedown delete print. "
                                 "Media (alone): play next prev stop mute volup voldown."
                               : why);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 368);
        lv_obj_set_pos(l, 0, 56);
        break;
    }
    case DP_PAGE: {
        const dp_deck_t *dk = dashpad_deck();
        int k = 0;
        for (int i = 0; i < dk->npages; i++) {
            if (i && i % 2 == 0) s_page_map[k++] = "\n";
            s_page_map[k++] = dk->p[i].name;
        }
        s_page_map[k] = "";
        lv_obj_t *m = matrix(d, s_page_map, 368, ((dk->npages + 1) / 2) * 42, det_page);
        for (int i = 0; i < dk->npages; i++) {
            if (!strcmp(dk->p[i].name, E.act.text)) lv_btnmatrix_set_btn_ctrl(m, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
        if (!E.act.text[0]) {
            snprintf(E.act.text, sizeof(E.act.text), "%s", dk->p[0].name);
            lv_btnmatrix_set_btn_ctrl(m, 0, LV_BTNMATRIX_CTRL_CHECKED);
        }
        break;
    }
    case DP_MULTI: {
        lv_obj_t *l = label(d, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");
        lv_label_set_text_fmt(l, "Multi-step action (%d steps). Edit the steps in a "
                              "dashpad.json file; choosing another type replaces it.", E.nsteps);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 368);
        break;
    }
    default: {
        lv_obj_t *l = label(d, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "This button does nothing.");
        (void)l;
        break;
    }
    }
}

static void editor_open(int index)
{
    if (U.ed) return;
    U.ed_index = index;
    E = page()->b[index];
    if (!E.used) {
        memset(&E, 0, sizeof(E));
        snprintf(E.symbol, sizeof(E.symbol), "play");
        E.color = COLORS[0];
    }
    lv_obj_t *o = U.ed = overlay();
    lv_obj_set_pos(label(o, APP_FONT_LARGE, APP_COLOR_TEXT,
                         page()->b[index].used ? "Edit button" : "New button"), 16, 12);
    lv_obj_align(button(o, LV_SYMBOL_OK "  Save", 120, 44, 0x1B8F4A, ed_save, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 6);
    lv_obj_align(button(o, "Cancel", 110, 44, C_BUTTON, ed_cancel, NULL),
                 LV_ALIGN_TOP_RIGHT, -140, 6);
    if (page()->b[index].used) {
        lv_obj_align(button(o, LV_SYMBOL_TRASH "  Delete", 130, 44, 0x8E2A2A, ed_delete, NULL),
                     LV_ALIGN_TOP_RIGHT, -258, 6);
    }

    /* Left: label, colour, symbol. */
    lv_obj_t *lf = field(o, E.label[0] ? E.label : "(no label)", 60, ed_label_clicked);
    lv_obj_set_x(lf, 16);
    U.ed_label = lv_obj_get_child(lf, 0);

    lv_obj_t *row = lv_obj_create(o);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 368, 32);
    lv_obj_set_pos(row, 16, 116);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < N_COLORS; i++) {
        lv_obj_t *sw = lv_obj_create(row);
        lv_obj_remove_style_all(sw);
        lv_obj_set_size(sw, 32, 32);
        lv_obj_set_pos(sw, i * 37, 0);
        lv_obj_set_style_radius(sw, 8, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(COLORS[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sw, lv_color_white(), 0);
        lv_obj_set_style_border_width(sw, COLORS[i] == E.color ? 3 : 0, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sw, ed_color, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    int k = 0, n = dashpad_symbol_count();
    for (int i = 0; i < n; i++) {
        if (i && i % 10 == 0) s_sym_map[k++] = "\n";
        s_sym_map[k++] = dashpad_symbol(dashpad_symbol_name(i));
    }
    s_sym_map[k] = "";
    U.ed_syms = matrix(o, s_sym_map, 368, 250, ed_symbol);
    lv_obj_set_pos(U.ed_syms, 16, 158);
    lv_obj_set_style_pad_gap(U.ed_syms, 3, 0);
    for (int i = 0; i < n; i++) {
        if (!strcmp(dashpad_symbol_name(i), E.symbol)) {
            lv_btnmatrix_set_btn_ctrl(U.ed_syms, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }

    /* Right: what the button does. */
    U.ed_kind = matrix(o, s_kind_map, 368, 138, ed_kind);
    lv_obj_set_pos(U.ed_kind, 416, 60);
    for (int i = 0; i < N_KINDS; i++) {
        if (KIND_OF[i] == E.act.kind) lv_btnmatrix_set_btn_ctrl(U.ed_kind, i, LV_BTNMATRIX_CTRL_CHECKED);
    }
    U.ed_detail = lv_obj_create(o);
    lv_obj_remove_style_all(U.ed_detail);
    lv_obj_set_size(U.ed_detail, 368, 200);
    lv_obj_set_pos(U.ed_detail, 416, 208);
    lv_obj_clear_flag(U.ed_detail, LV_OBJ_FLAG_SCROLLABLE);
    editor_refresh_detail();
}

/* ======================= menu ======================= */

static char s_name[DP_LABEL];

static void menu_close(void)
{
    if (U.menu) lv_obj_del_async(U.menu);
    U.menu = NULL;
}

static void menu_cancel(lv_event_t *e) { (void)e; menu_close(); }

static bool name_taken(const char *name, int except)
{
    const dp_deck_t *d = dashpad_deck();
    for (int i = 0; i < d->npages; i++) {
        if (i != except && !strcmp(d->p[i].name, name)) return true;
    }
    return false;
}

static void add_page_done(void)
{
    dp_deck_t *d = dashpad_deck();
    if (!s_name[0] || name_taken(s_name, -1)) {
        launcher_toast("Pick a name that isn't used yet", 4000);
        return;
    }
    memset(&d->p[d->npages], 0, sizeof(dp_page_t));
    snprintf(d->p[d->npages].name, DP_LABEL, "%s", s_name);
    U.page = d->npages++;
    U.edit = true;                                /* show the empty slots */
    save();
    menu_close();
    rebuild_grid();
}

static void add_page(lv_event_t *e)
{
    (void)e;
    if (dashpad_deck()->npages >= DP_PAGES) {
        launcher_toast("DashPad has 8 pages already", 4000);
        return;
    }
    snprintf(s_name, sizeof(s_name), "Page %d", dashpad_deck()->npages + 1);
    prompt("Name for the new page", s_name, sizeof(s_name), add_page_done);
}

static void rename_done(void)
{
    dp_deck_t *d = dashpad_deck();
    if (!s_name[0] || name_taken(s_name, U.page)) {
        launcher_toast("Pick a name that isn't used yet", 4000);
        return;
    }
    /* Buttons that open this page follow the new name. */
    for (int p = 0; p < d->npages; p++) {
        for (int b = 0; b < DP_BUTTONS; b++) {
            dp_action_t *a = &d->p[p].b[b].act;
            if (a->kind == DP_PAGE && !strcmp(a->text, page()->name)) {
                snprintf(a->text, sizeof(a->text), "%s", s_name);
            }
        }
    }
    snprintf(page()->name, DP_LABEL, "%s", s_name);
    save();
    menu_close();
    rebuild_grid();
}

static void rename_page(lv_event_t *e)
{
    (void)e;
    snprintf(s_name, sizeof(s_name), "%s", page()->name);
    prompt("Rename this page", s_name, sizeof(s_name), rename_done);
}

static void delete_page_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (!yes) return;
    dp_deck_t *d = dashpad_deck();
    char gone[DP_LABEL];
    snprintf(gone, sizeof(gone), "%s", page()->name);
    memmove(&d->p[U.page], &d->p[U.page + 1], sizeof(dp_page_t) * (d->npages - U.page - 1));
    d->npages--;
    for (int p = 0; p < d->npages; p++) {               /* buttons that opened it: nothing */
        for (int b = 0; b < DP_BUTTONS; b++) {
            dp_action_t *a = &d->p[p].b[b].act;
            if (a->kind == DP_PAGE && !strcmp(a->text, gone)) memset(a, 0, sizeof(*a));
        }
    }
    if (U.page >= d->npages) U.page = d->npages - 1;
    save();
    menu_close();
    rebuild_grid();
}

static void delete_page(lv_event_t *e)
{
    (void)e;
    if (dashpad_deck()->npages < 2) {
        launcher_toast("The last page can't be deleted", 4000);
        return;
    }
    char t[64];
    snprintf(t, sizeof(t), "Delete the page \"%s\"?", page()->name);
    lv_obj_t *row = game_dialog_open(U.root, t);
    game_dialog_button(row, LV_SYMBOL_TRASH "  Delete", delete_page_answer, (void *)1);
    game_dialog_button(row, "Cancel", delete_page_answer, NULL);
}

static void export_clicked(lv_event_t *e)
{
    (void)e;
    char path[96], msg[140];
    if (dashpad_export(path, sizeof(path))) {
        snprintf(msg, sizeof(msg), "Saved to %s", strstr(path, "dashboard/") ? strstr(path, "dashboard/") : path);
    } else {
        snprintf(msg, sizeof(msg), "Couldn't write to the SD card");
    }
    launcher_toast(msg, 5000);
}

static void reset_answer(lv_event_t *e)
{
    bool yes = lv_event_get_user_data(e) != NULL;
    game_dialog_close();
    if (!yes) return;
    dashpad_reset_default();
    U.page = 0;
    menu_close();
    rebuild_grid();
}

static void reset_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_t *row = game_dialog_open(U.root, "Replace the layout with the example?");
    game_dialog_button(row, "Replace", reset_answer, (void *)1);
    game_dialog_button(row, "Cancel", reset_answer, NULL);
}

static void unpair_clicked(lv_event_t *e)
{
    (void)e;
    dashpad_unpair();
    menu_close();
}

static lv_obj_t *checkbox(lv_obj_t *p, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *c = lv_checkbox_create(p);
    lv_checkbox_set_text(c, text);
    lv_obj_set_style_text_font(c, APP_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(c, lv_color_hex(APP_COLOR_TEXT), 0);
    lv_obj_set_style_pad_column(c, 12, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_BUTTON), LV_PART_INDICATOR);
    lv_obj_set_style_border_color(c, lv_color_hex(0x5F6B77), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(c, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(c, LV_STATE_CHECKED);
    lv_obj_add_event_cb(c, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return c;
}

static void pc_toggled(lv_event_t *e)
{
    dashpad_set_helper_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    update_pc();
}

static void kbd_toggled(lv_event_t *e)
{
    dashpad_set_keyboard_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    update_keyboard();
    update_pc();
}

static void menu_clicked(lv_event_t *e)
{
    (void)e;
    if (U.menu) return;
    lv_obj_t *o = U.menu = overlay();
    lv_obj_set_pos(label(o, APP_FONT_LARGE, APP_COLOR_TEXT, "DashPad"), 16, 12);
    lv_obj_align(button(o, LV_SYMBOL_CLOSE "  Close", 120, 44, C_BUTTON, menu_cancel, NULL),
                 LV_ALIGN_TOP_RIGHT, -12, 6);
    static const struct { const char *text; lv_event_cb_t cb; } items[] = {
        { LV_SYMBOL_PLUS "  Add a page", add_page },
        { LV_SYMBOL_EDIT "  Rename this page", rename_page },
        { LV_SYMBOL_TRASH "  Delete this page", delete_page },
        { LV_SYMBOL_SAVE "  Export to the SD card", export_clicked },
        { LV_SYMBOL_REFRESH "  Reset to the example", reset_clicked },
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = button(o, items[i].text, 330, 52, C_BUTTON, items[i].cb, NULL);
        lv_obj_set_pos(b, 16, 66 + i * 64);
        lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_LEFT_MID, 6, 0);
    }

    /* Connections: how buttons reach the PC. */
    lv_obj_set_pos(label(o, APP_FONT_MEDIUM, APP_COLOR_TEXT, "Connections"), 372, 68);
    char host[64], text[300];
    int port;
    bool paired = dashpad_paired(host, sizeof(host), &port);
    lv_obj_t *pc = checkbox(o, "PC helper", dashpad_helper_on(), pc_toggled);
    lv_obj_set_pos(pc, 372, 100);
    if (paired) {
        snprintf(text, sizeof(text), "Runs \"Run on PC\" buttons. Paired with %s:%d; the display "
                 "only sends action names, signed with the shared secret, and the PC runs only "
                 "what its helper.json lists.", host, port);
        lv_obj_t *b = button(o, "Unpair", 100, 38, 0x5D1F1F, unpair_clicked, NULL);
        lv_obj_set_pos(b, 684, 96);
    } else {
        snprintf(text, sizeof(text), "Runs \"Run on PC\" buttons. Not paired: on the PC run "
                 "tools/dashpad_helper.py serve, then (with DashPad closed) dashpad_helper.py "
                 "pair, which sends the details over the USB cable.");
    }
    lv_obj_t *info = label(o, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT, text);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info, 412);
    lv_obj_set_pos(info, 372, 142);

    lv_obj_t *kb = checkbox(o, "USB keyboard", dashpad_keyboard_on(), kbd_toggled);
    lv_obj_set_pos(kb, 372, 240);
    lv_obj_t *note = label(o, LV_FONT_DEFAULT, APP_COLOR_SUBTEXT,
                           "Sends \"Keyboard keys\" buttons, and the keys of \"Run on PC\" "
                           "buttons when the helper is off. While DashPad is open the USB port "
                           "is a keyboard called \"DashPad\"; flashing, the log and the USB file "
                           "tool come back when you leave DashPad (or restart the display).");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, 412);
    lv_obj_set_pos(note, 372, 282);
}

/* ======================= app ======================= */

static void dashpad_create(lv_obj_t *parent)
{
    memset(&U, 0, sizeof(U));
    U.root = parent;
    U.grid = lv_obj_create(parent);
    lv_obj_remove_style_all(U.grid);
    lv_obj_set_size(U.grid, AREA_W, AREA_H - BAR_H);
    lv_obj_clear_flag(U.grid, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, AREA_W, BAR_H);
    lv_obj_set_pos(bar, 0, AREA_H - BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(button(bar, LV_SYMBOL_LEFT, 56, 42, C_BUTTON, prev_clicked, NULL),
                 LV_ALIGN_LEFT_MID, 8, 0);
    U.page_lbl = label(bar, APP_FONT_MEDIUM, APP_COLOR_TEXT, "");
    lv_label_set_long_mode(U.page_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(U.page_lbl, 250);
    lv_obj_set_style_text_align(U.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(U.page_lbl, LV_ALIGN_LEFT_MID, 72, 0);
    lv_obj_align(button(bar, LV_SYMBOL_RIGHT, 56, 42, C_BUTTON, next_clicked, NULL),
                 LV_ALIGN_LEFT_MID, 330, 0);
    U.pc_lbl = label(bar, APP_FONT_MEDIUM, APP_COLOR_SUBTEXT, "");
    lv_obj_align(U.pc_lbl, LV_ALIGN_LEFT_MID, 404, 0);
    U.edit_btn = button(bar, LV_SYMBOL_EDIT "  Edit", 110, 42, C_BUTTON, edit_clicked, NULL);
    lv_obj_align(U.edit_btn, LV_ALIGN_RIGHT_MID, -76, 0);
    lv_obj_align(button(bar, LV_SYMBOL_BARS, 56, 42, C_BUTTON, menu_clicked, NULL),
                 LV_ALIGN_RIGHT_MID, -8, 0);

    rebuild_grid();
    update_pc();
    dashpad_set_active(true);
    U.timer = lv_timer_create(tick, 100, NULL);
}

static void dashpad_destroy(void)
{
    dashpad_set_active(false);
    usb_kbd_stop();                               /* the USB port back to flashing/log */
    if (U.timer) lv_timer_del(U.timer);
    game_dialog_close();
    memset(&U, 0, sizeof(U));                     /* the launcher deletes the objects */
}

const app_t dashpad_app = {
    .name      = "DashPad",
    .symbol    = LV_SYMBOL_KEYBOARD,
    .color_hex = 0x00695C,
    .create    = dashpad_create,
    .destroy   = dashpad_destroy,
};
