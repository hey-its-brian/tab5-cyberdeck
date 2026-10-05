/*
 * NOTES module: markdown file browser, editor and live preview.
 *
 *   browser   list of .md files, newest first; open, new, rename, delete
 *   document  EDIT (textarea), VIEW (rendered) or SPLIT (both, live)
 *
 * Keys in the browser: Enter open, Ctrl+N new, Ctrl+R rename, Del delete.
 * Keys in a document: Ctrl+S save, Ctrl+P edit/preview, Ctrl+T split,
 * Esc save and close. Autosaves every 20 s while there are changes.
 */
#include "app_notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_md.h"
#include "deck_modal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "notes_store.h"

#define MAX_NOTES 64
#define AUTOSAVE_MS 20000
#define PREVIEW_DEBOUNCE_MS 350

#define HID_N 0x11
#define HID_P 0x13
#define HID_R 0x15
#define HID_S 0x16
#define HID_T 0x17

typedef enum { MODE_EDIT, MODE_SPLIT, MODE_VIEW } mode_t_;

static struct {
    lv_obj_t *parent;     /* module content area */
    lv_obj_t *screen;     /* current browser or document root */

    /* browser */
    note_entry_t notes[MAX_NOTES];
    int count;

    /* document */
    bool doc_open;
    char name[NOTES_NAME_MAX];
    mode_t_ mode;
    bool dirty;
    lv_obj_t *title;
    lv_obj_t *status;
    lv_obj_t *mode_btn[3];
    lv_obj_t *ta;
    lv_obj_t *preview;
    lv_obj_t *osk;
    lv_timer_t *autosave;
    lv_timer_t *debounce;
    uint32_t status_until;
    uint32_t dirty_since;
} s_n;

static void show_browser(const char *focus_name);
static void open_doc(const char *name, mode_t_ mode);

/* ---- Small helpers ------------------------------------------------------- */

static lv_obj_t *page(void)
{
    /* Usually called from a click inside the old page, so delete it later. */
    if (s_n.screen) {
        lv_obj_add_flag(s_n.screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(s_n.screen);
    }
    lv_group_remove_all_objs(deck_input_group());
    s_n.screen = deck_box(s_n.parent);
    lv_obj_set_size(s_n.screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_n.screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_n.screen, 20, 0);
    lv_obj_set_style_pad_row(s_n.screen, 14, 0);
    return s_n.screen;
}

/* A button that keyboard focus skips (keys have shortcuts instead). */
static lv_obj_t *tool_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = deck_button(parent, text, g_font.disp_s);
    lv_obj_set_style_min_height(b, 46, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_group_remove_obj(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void fmt_size(char *buf, size_t n, long size)
{
    if (size < 1024) {
        snprintf(buf, n, "%ld B", size);
    } else {
        snprintf(buf, n, "%.1f KB", (double)size / 1024.0);
    }
}

/* ---- Browser actions ----------------------------------------------------- */

static void new_named(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL) return;
    char name[NOTES_NAME_MAX];
    if (!notes_sanitize(text, name, sizeof(name))) return;
    if (!notes_exists(name)) {
        char body[96];
        char title[NOTES_NAME_MAX];
        snprintf(title, sizeof(title), "%s", name);
        title[strlen(title) - 3] = '\0'; /* drop .md */
        int len = snprintf(body, sizeof(body), "# %s\n\n", title);
        notes_write(name, body, (size_t)len);
    }
    open_doc(name, MODE_EDIT);
}

static void new_note(void) { deck_modal_prompt("NEW NOTE", "", new_named, NULL); }

static char s_pending[NOTES_NAME_MAX];

static void rename_named(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL) return;
    char to[NOTES_NAME_MAX];
    if (notes_sanitize(text, to, sizeof(to)) && strcmp(to, s_pending) != 0 && notes_rename(s_pending, to)) {
        show_browser(to);
    } else {
        show_browser(s_pending);
    }
}

static void rename_note(const char *name)
{
    snprintf(s_pending, sizeof(s_pending), "%s", name);
    char initial[NOTES_NAME_MAX];
    snprintf(initial, sizeof(initial), "%s", name);
    initial[strlen(initial) - 3] = '\0';
    deck_modal_prompt("RENAME", initial, rename_named, NULL);
}

static void delete_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (yes) notes_delete(s_pending);
    show_browser(NULL);
}

static void delete_note(const char *name)
{
    snprintf(s_pending, sizeof(s_pending), "%s", name);
    char msg[128];
    snprintf(msg, sizeof(msg), "Permanently delete %s?", name);
    deck_modal_confirm("DELETE", msg, "DELETE", delete_confirmed, NULL);
}

/* Which note row has keyboard focus, if any. */
static const char *focused_note(void)
{
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    if (f == NULL || s_n.doc_open) return NULL;
    intptr_t i = (intptr_t)lv_obj_get_user_data(f);
    return (i >= 1 && i <= s_n.count) ? s_n.notes[i - 1].name : NULL;
}

static void row_clicked(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    open_doc(s_n.notes[i - 1].name, MODE_VIEW);
}

static void row_rename(lv_event_t *e) { rename_note(s_n.notes[(intptr_t)lv_event_get_user_data(e) - 1].name); }
static void row_delete(lv_event_t *e) { delete_note(s_n.notes[(intptr_t)lv_event_get_user_data(e) - 1].name); }
static void new_clicked(lv_event_t *e)
{
    (void)e;
    new_note();
}

/* ---- Browser ------------------------------------------------------------- */

static void show_browser(const char *focus_name)
{
    s_n.doc_open = false;
    lv_obj_t *root = page();

    const char *dir = notes_dir();
    s_n.count       = dir ? notes_list(s_n.notes, MAX_NOTES) : 0;

    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, "// DATASTORE");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    char info[128];
    snprintf(info, sizeof(info), "%s  ::  %d FILE%s  ::  %s", dir ? dir : "NO STORAGE", s_n.count,
             s_n.count == 1 ? "" : "S", hal_storage_is_sd() ? "SD" : "INTERNAL FLASH");
    lv_obj_t *path = deck_label(head, g_font.mono_s, g_pal.dim, info);
    lv_obj_align(path, LV_ALIGN_LEFT_MID, 190, 0);
    lv_obj_t *nb = tool_button(head, "+ NEW", new_clicked, NULL);
    lv_obj_align(nb, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *list = deck_box(root);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_style_pad_all(list, 6, 0); /* room for the focus glow */
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *focus = NULL;
    if (dir == NULL) {
        deck_label(list, g_font.mono_m, g_pal.warn, "No SD card and no internal storage. Insert a card.");
    } else if (s_n.count == 0) {
        deck_label(list, g_font.mono_m, g_pal.dim, "Empty. Ctrl+N or + NEW to start a note.");
    }
    for (int i = 0; i < s_n.count; i++) {
        const note_entry_t *n = &s_n.notes[i];
        lv_obj_t *row         = deck_panel(list, DECK_CUT_BR, 12);
        lv_obj_set_size(row, LV_PCT(100), 68);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_pad_hor(row, 18, 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)(i + 1));
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        lv_group_add_obj(deck_input_group(), row);

        lv_obj_t *nm = deck_icon_text(row, ICON_MARKDOWN, n->name, g_font.mono_l, g_pal.text);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        char meta[64], sz[16];
        struct tm tm;
        localtime_r(&n->mtime, &tm);
        fmt_size(sz, sizeof(sz), n->size);
        snprintf(meta, sizeof(meta), "%s   %04d-%02d-%02d %02d:%02d", sz, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min);
        lv_obj_t *m = deck_label(row, g_font.mono_s, g_pal.dim, meta);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, -220, 0);

        lv_obj_t *del = tool_button(row, "DEL", row_delete, (void *)(intptr_t)(i + 1));
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t *ren = tool_button(row, "REN", row_rename, (void *)(intptr_t)(i + 1));
        lv_obj_align(ren, LV_ALIGN_RIGHT_MID, -106, 0);

        if (focus == NULL || (focus_name && strcmp(focus_name, n->name) == 0)) focus = row;
    }

    deck_label(root, g_font.mono_s, g_pal.dim,
               "[ENTER] OPEN    [CTRL+N] NEW    [CTRL+R] RENAME    [DEL] DELETE    [ESC] DECK");

    if (focus) {
        lv_group_focus_obj(focus);
        lv_obj_add_state(focus, LV_STATE_FOCUS_KEY); /* show it; LVGL only sets this for real key focus */
    }
}

/* ---- Document ------------------------------------------------------------ */

static void flash_status(const char *text, lv_color_t color)
{
    lv_label_set_text(s_n.status, text);
    lv_obj_set_style_text_color(s_n.status, color, 0);
    s_n.status_until = lv_tick_get() + 1500;
}

static void update_title(void)
{
    lv_label_set_text_fmt(s_n.title, "%s%s", s_n.name, s_n.dirty ? " *" : "");
}

static bool save(void)
{
    if (!s_n.doc_open || !s_n.dirty) return true;
    const char *txt = lv_textarea_get_text(s_n.ta);
    bool ok         = notes_write(s_n.name, txt, strlen(txt));
    if (ok) s_n.dirty = false;
    update_title();
    flash_status(ok ? "SAVED" : "SAVE FAILED", ok ? g_pal.ok : g_pal.danger);
    return ok;
}

static void render_preview(void)
{
    if (s_n.preview == NULL) return;
    const char *txt = lv_textarea_get_text(s_n.ta);
    int32_t y       = lv_obj_get_scroll_y(s_n.preview);
    deck_md_render(s_n.preview, txt, strlen(txt));
    lv_obj_update_layout(s_n.preview);
    lv_obj_scroll_to_y(s_n.preview, y, LV_ANIM_OFF);
}

static void apply_mode(mode_t_ mode)
{
    s_n.mode = mode;
    bool edit = mode != MODE_VIEW, view = mode != MODE_EDIT;

    if (edit) {
        lv_obj_remove_flag(s_n.ta, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_n.ta, LV_OBJ_FLAG_HIDDEN);
    }
    if (view) {
        lv_obj_remove_flag(s_n.preview, LV_OBJ_FLAG_HIDDEN);
        render_preview();
    } else {
        lv_obj_add_flag(s_n.preview, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_n.osk) {
        if (edit) {
            lv_obj_remove_flag(s_n.osk, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_n.osk, LV_OBJ_FLAG_HIDDEN);
        }
    }
    for (int i = 0; i < 3; i++) {
        if (i == (int)mode) {
            lv_obj_add_state(s_n.mode_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_n.mode_btn[i], LV_STATE_CHECKED);
        }
    }
    if (edit) lv_group_focus_obj(s_n.ta);
}

static void close_doc(void)
{
    char name[NOTES_NAME_MAX];
    snprintf(name, sizeof(name), "%s", s_n.name);
    save();
    if (s_n.autosave) lv_timer_delete(s_n.autosave);
    if (s_n.debounce) lv_timer_delete(s_n.debounce);
    s_n.autosave = s_n.debounce = NULL;
    s_n.ta = s_n.preview = s_n.osk = NULL;
    show_browser(name);
}

static void debounce_fire(lv_timer_t *t)
{
    (void)t;
    s_n.debounce = NULL; /* one-shot timers delete themselves */
    render_preview();
}

static void ta_changed(lv_event_t *e)
{
    (void)e;
    if (!s_n.dirty) {
        s_n.dirty       = true;
        s_n.dirty_since = lv_tick_get();
        update_title();
    }
    if (s_n.mode == MODE_SPLIT) {
        if (s_n.debounce) {
            lv_timer_reset(s_n.debounce);
        } else {
            s_n.debounce = lv_timer_create(debounce_fire, PREVIEW_DEBOUNCE_MS, NULL);
            lv_timer_set_repeat_count(s_n.debounce, 1);
        }
    }
}

static void autosave_tick(lv_timer_t *t)
{
    (void)t;
    if (s_n.dirty && lv_tick_elaps(s_n.dirty_since) >= AUTOSAVE_MS) save();
    if (s_n.status_until && lv_tick_get() > s_n.status_until) {
        lv_label_set_text(s_n.status, "");
        s_n.status_until = 0;
    }
}

static void mode_clicked(lv_event_t *e) { apply_mode((mode_t_)(intptr_t)lv_event_get_user_data(e)); }
static void save_clicked(lv_event_t *e)
{
    (void)e;
    s_n.dirty = true; /* explicit save always writes */
    save();
}
static void close_clicked(lv_event_t *e)
{
    (void)e;
    close_doc();
}

static void open_doc(const char *name, mode_t_ mode)
{
    size_t len = 0;
    char *text = notes_read(name, &len);
    if (text == NULL) {
        show_browser(NULL);
        return;
    }

    s_n.doc_open = true;
    s_n.dirty    = false;
    snprintf(s_n.name, sizeof(s_n.name), "%s", name);
    lv_obj_t *root = page();

    /* Toolbar */
    lv_obj_t *bar = deck_box(root);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, 0);
    lv_obj_t *icon = deck_label(bar, g_font.icon_s, g_pal.accent, ICON_MARKDOWN);
    (void)icon;
    s_n.title = deck_label(bar, g_font.mono_l, g_pal.text, "");
    s_n.status = deck_label(bar, g_font.mono_s, g_pal.ok, "");
    lv_obj_set_flex_grow(s_n.status, 1);
    static const char *labels[3] = {"EDIT", "SPLIT", "VIEW"};
    for (int i = 0; i < 3; i++) {
        s_n.mode_btn[i] = tool_button(bar, labels[i], mode_clicked, (void *)(intptr_t)i);
    }
    tool_button(bar, "SAVE", save_clicked, NULL);
    tool_button(bar, "CLOSE", close_clicked, NULL);
    update_title();

    /* Body: editor and preview side by side; mode decides who is visible. */
    lv_obj_t *body = deck_box(root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 16, 0);

    s_n.ta = lv_textarea_create(body);
    lv_obj_set_height(s_n.ta, LV_PCT(100));
    lv_obj_set_flex_grow(s_n.ta, 1);
    lv_obj_set_style_text_font(s_n.ta, g_font.mono_m, 0);
    lv_obj_set_style_text_color(s_n.ta, g_pal.text, 0);
    lv_obj_set_style_text_line_space(s_n.ta, 4, 0);
    lv_obj_set_style_bg_color(s_n.ta, lv_color_hex(0x04050A), 0);
    lv_obj_set_style_border_color(s_n.ta, g_pal.line, 0);
    lv_obj_set_style_border_color(s_n.ta, g_pal.accent, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_n.ta, 0, 0);
    lv_obj_set_style_pad_all(s_n.ta, 16, 0);
    lv_obj_set_style_bg_color(s_n.ta, g_pal.accent, LV_PART_CURSOR);
    lv_textarea_set_text(s_n.ta, text);
    lv_textarea_set_cursor_pos(s_n.ta, mode == MODE_EDIT ? LV_TEXTAREA_CURSOR_LAST : 0);
    lv_obj_add_event_cb(s_n.ta, ta_changed, LV_EVENT_VALUE_CHANGED, NULL);
    free(text);

    s_n.preview = deck_panel(body, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_height(s_n.preview, LV_PCT(100));
    lv_obj_set_flex_grow(s_n.preview, 1);
    lv_obj_set_style_pad_all(s_n.preview, 24, 0);
    lv_obj_add_flag(s_n.preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_n.preview, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_n.preview, LV_SCROLLBAR_MODE_AUTO);

    /* Touch-only (no keyboard attached): on-screen keyboard while editing. */
    s_n.osk = NULL;
    if (!hal_kbd_present()) {
        s_n.osk = lv_keyboard_create(root);
        lv_obj_set_size(s_n.osk, LV_PCT(100), LV_PCT(40));
        lv_keyboard_set_textarea(s_n.osk, s_n.ta);
        lv_group_remove_obj(s_n.osk);
    }

    lv_group_add_obj(deck_input_group(), s_n.ta);
    s_n.autosave = lv_timer_create(autosave_tick, 500, NULL);
    s_n.status_until = 0;
    apply_mode(mode);
}

/* ---- Keys ---------------------------------------------------------------- */

static bool doc_key(const deck_key_t *k)
{
    bool ctrl = (k->mods & DECK_MOD_CTRL) != 0;
    if (ctrl && k->code == HID_S) {
        s_n.dirty = true;
        save();
        return true;
    }
    if (ctrl && k->code == HID_P) {
        apply_mode(s_n.mode == MODE_VIEW ? MODE_EDIT : MODE_VIEW);
        return true;
    }
    if (ctrl && k->code == HID_T) {
        apply_mode(s_n.mode == MODE_SPLIT ? MODE_EDIT : MODE_SPLIT);
        return true;
    }
    if (k->code == HID_ESC) {
        close_doc();
        return true;
    }
    if (s_n.mode == MODE_VIEW) {
        /* Nothing is focused in pure preview: arrows and space scroll it. */
        int32_t page = lv_obj_get_height(s_n.preview) - 80;
        int32_t dy   = k->code == HID_DOWN ? 80 : k->code == HID_UP ? -80 : k->code == HID_SPACE ? page : 0;
        if (dy) lv_obj_scroll_by_bounded(s_n.preview, 0, -dy, LV_ANIM_ON);
        return dy != 0;
    }
    if (k->code == HID_TAB && !ctrl) {
        lv_textarea_add_text(s_n.ta, "  "); /* indent instead of moving focus */
        return true;
    }
    return false;
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (s_n.doc_open) return doc_key(k);

    bool ctrl = (k->mods & DECK_MOD_CTRL) != 0;
    if (ctrl && k->code == HID_N) {
        new_note();
        return true;
    }
    const char *f = focused_note();
    if (f && ctrl && k->code == HID_R) {
        rename_note(f);
        return true;
    }
    if (f && k->code == HID_DELETE) {
        delete_note(f);
        return true;
    }
    return false;
}

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_n, 0, sizeof(s_n));
    s_n.parent = parent;
    notes_seed();
    show_browser(NULL);
    return true;
}

/* Leaving the module (Esc from the browser, Alt+Esc, Alt+N): the editor
 * still exists here, so this is the last chance to save. */
static void notes_exit(deck_app_t *self)
{
    (void)self;
    deck_modal_discard();
    if (s_n.doc_open) save();
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_n.autosave) lv_timer_delete(s_n.autosave);
    if (s_n.debounce) lv_timer_delete(s_n.debounce);
    memset(&s_n, 0, sizeof(s_n));
}

static deck_app_t s_app = {
    .name     = "NOTES",
    .tagline  = "MARKDOWN I/O",
    .icon     = ICON_MARKDOWN,
    .eta      = NULL,
    .on_start = start,
    .on_exit  = notes_exit,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_notes(void) { return &s_app; }
