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

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_md.h"
#include "deck_modal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "deck_sync.h"
#include "notes_store.h"

#define MAX_NOTES 128
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

    /* sync panel */
    bool sync_open;
    lv_obj_t *sync_status;
    lv_obj_t *sync_log;
    lv_obj_t *sync_btn;
    lv_timer_t *sync_timer;
    sync_state_t sync_seen;

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
} s_n DECK_PSRAM_BSS;

static void show_browser(const char *focus_name);
static void open_doc(const char *name, mode_t_ mode);
static void show_sync(void);

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
/* The focused row's note, or NULL when a folder or nothing is focused. */
static const char *focused_note(void)
{
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    if (f == NULL || s_n.doc_open) return NULL;
    intptr_t i = (intptr_t)lv_obj_get_user_data(f);
    return (i >= 1 && i <= s_n.count && !s_n.notes[i - 1].is_dir) ? s_n.notes[i - 1].name : NULL;
}

static void row_clicked(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (s_n.notes[i - 1].is_dir) {
        if (notes_enter(s_n.notes[i - 1].name)) show_browser(NULL);
        return;
    }
    open_doc(s_n.notes[i - 1].name, MODE_VIEW);
}

static void go_up(void)
{
    char was[NOTES_NAME_MAX] = "";
    const char *slash        = strrchr(notes_subdir(), '/');
    snprintf(was, sizeof(was), "%s", slash ? slash + 1 : notes_subdir());
    if (notes_up()) show_browser(was[0] ? was : NULL);
}

static void source_clicked(lv_event_t *e)
{
    (void)e;
    notes_set_vault_mode(!notes_vault_mode());
    show_browser(NULL);
}

static void sync_clicked(lv_event_t *e)
{
    (void)e;
    show_sync();
}

static void sync_refresh(lv_timer_t *t);

/* Half-second tick: the sync panel redraws its status and log; the browser
 * reloads itself when a sync that was running finishes. */
static void browser_sync_tick(lv_timer_t *t)
{
    (void)t;
    if (s_n.sync_open) {
        sync_refresh(NULL);
        return;
    }
    sync_state_t st = deck_sync_state();
    if (st != s_n.sync_seen) {
        s_n.sync_seen = st;
        if (st == SYNC_DONE && !s_n.doc_open) show_browser(NULL);
    }
}

/* ---- Sync panel ---------------------------------------------------------- */

static void setting_entered(const char *text, void *ud)
{
    const char *key = (const char *)ud;
    if (text == NULL) return;
    hal_cfg_set_str(key, text[0] ? text : NULL);
    if (strcmp(key, SYNC_KEY_PHRASE) == 0) hal_cfg_set_str("ls_mkey", NULL); /* the cached key is for the old passphrase */
    show_sync();
}

static void edit_setting(lv_event_t *e)
{
    const char *key = (const char *)lv_event_get_user_data(e);
    char cur[160] = "";
    hal_cfg_get_str(key, cur, sizeof(cur));
    if (strcmp(key, SYNC_KEY_PASS) == 0) {
        deck_modal_password("COUCHDB PASSWORD", setting_entered, (void *)key);
    } else if (strcmp(key, SYNC_KEY_PHRASE) == 0) {
        deck_modal_password("E2EE PASSPHRASE (EMPTY = OFF)", setting_entered, (void *)key);
    } else if (strcmp(key, SYNC_KEY_URL) == 0) {
        deck_modal_prompt("SERVER (http://host:5984)", cur, setting_entered, (void *)key);
    } else if (strcmp(key, SYNC_KEY_DB) == 0) {
        deck_modal_prompt("DATABASE", cur, setting_entered, (void *)key);
    } else {
        deck_modal_prompt("USER", cur, setting_entered, (void *)key);
    }
}

static const int s_auto_opts[] = {0, 5, 15, 60};

static void auto_clicked(lv_event_t *e)
{
    int cur = (int)hal_cfg_get_i32(SYNC_KEY_AUTO, 0), next = 0;
    for (int i = 0; i < 4; i++) {
        if (s_auto_opts[i] == cur) next = s_auto_opts[(i + 1) % 4];
    }
    hal_cfg_set_i32(SYNC_KEY_AUTO, next);
    char b[16];
    snprintf(b, sizeof(b), next ? "%d MIN" : "OFF", next);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0), b);
}

static void sync_now_clicked(lv_event_t *e)
{
    (void)e;
    if (deck_sync_state() == SYNC_RUNNING) {
        deck_sync_cancel();
    } else if (!deck_sync_configured()) {
        if (s_n.sync_status) {
            lv_label_set_text(s_n.sync_status, "SET THE SERVER AND DATABASE");
            lv_obj_set_style_text_color(s_n.sync_status, g_pal.warn, 0);
        }
    } else {
        deck_sync_start();
    }
}

static void sync_back_clicked(lv_event_t *e)
{
    (void)e;
    show_browser(NULL);
}

static void rescan_clicked(lv_event_t *e)
{
    (void)e;
    if (deck_sync_state() != SYNC_RUNNING && deck_sync_configured()) deck_sync_start_full();
}

static void sync_refresh(lv_timer_t *t)
{
    (void)t;
    if (!s_n.sync_open) return;
    sync_state_t st = deck_sync_state();
    const char *txt;
    lv_color_t col = g_pal.dim;
    char buf[64];
    switch (st) {
        case SYNC_RUNNING:
            txt = "SYNCING...";
            col = g_pal.accent;
            break;
        case SYNC_DONE: {
            long age = deck_sync_age();
            snprintf(buf, sizeof(buf), age < 90 ? "SYNCED JUST NOW" : "SYNCED %ld MIN AGO", age / 60);
            txt = buf;
            col = g_pal.ok;
            break;
        }
        case SYNC_FAILED:
            txt = "SYNC FAILED";
            col = g_pal.danger;
            break;
        default:
            txt = deck_sync_configured() ? "READY" : "NOT CONFIGURED";
            break;
    }
    lv_label_set_text(s_n.sync_status, txt);
    lv_obj_set_style_text_color(s_n.sync_status, col, 0);
    lv_label_set_text(lv_obj_get_child(s_n.sync_btn, 0), st == SYNC_RUNNING ? "STOP" : "SYNC NOW");

    static char lines[24][96]; /* off the UI task's stack */
    int n = deck_sync_log(lines, 24);
    static char all[24 * 100];
    size_t o = 0;
    for (int i = 0; i < n && o < sizeof(all) - 100; i++) {
        o += (size_t)snprintf(all + o, sizeof(all) - o, "%s> %s", i ? "\n" : "", lines[i]);
    }
    lv_label_set_text(s_n.sync_log, n ? all : "> no sync yet this boot");
    lv_obj_scroll_to_y(lv_obj_get_parent(s_n.sync_log), LV_COORD_MAX, LV_ANIM_OFF);
}

static lv_obj_t *sync_row(lv_obj_t *parent, const char *key, const char *label, bool secret)
{
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 50);
    deck_label(row, g_font.mono_m, g_pal.dim, label);
    char cur[160] = "";
    hal_cfg_get_str(key, cur, sizeof(cur));
    lv_obj_t *val = deck_label(row, g_font.mono_m, cur[0] ? g_pal.text : g_pal.warn,
                               cur[0] ? (secret ? "********" : cur) : "NOT SET");
    lv_label_set_long_mode(val, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(val, 300);
    lv_obj_align(val, LV_ALIGN_LEFT_MID, 150, 0);
    lv_obj_t *b = deck_button(row, "EDIT", g_font.disp_s);
    lv_obj_set_style_min_height(b, 40, 0);
    lv_obj_set_style_pad_ver(b, 6, 0);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(b, edit_setting, LV_EVENT_CLICKED, (void *)key);
    return row;
}

static void show_sync(void)
{
    s_n.doc_open  = false;
    s_n.sync_open = true;
    lv_obj_t *root = page();

    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, "// VAULT SYNC");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sub = deck_label(head, g_font.mono_s, g_pal.dim, "OBSIDIAN SELF-HOSTED LIVESYNC (COUCHDB)");
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 190, 0);
    lv_obj_t *back = tool_button(head, "BACK", sync_back_clicked, NULL);
    lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *cols = deck_box(root);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_flex_grow(cols, 1);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 20, 0);

    lv_obj_t *left = deck_panel(cols, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_size(left, 600, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left, 20, 0);
    lv_obj_set_style_pad_row(left, 6, 0);
    deck_section(left, "SERVER");
    sync_row(left, SYNC_KEY_URL, "SERVER", false);
    sync_row(left, SYNC_KEY_DB, "DATABASE", false);
    sync_row(left, SYNC_KEY_USER, "USER", false);
    sync_row(left, SYNC_KEY_PASS, "PASSWORD", true);
    sync_row(left, SYNC_KEY_PHRASE, "PASSPHRASE", true);
    lv_obj_t *arow = deck_box(left);
    lv_obj_set_width(arow, LV_PCT(100));
    lv_obj_set_height(arow, 50);
    deck_label(arow, g_font.mono_m, g_pal.dim, "AUTO SYNC");
    int auto_min = (int)hal_cfg_get_i32(SYNC_KEY_AUTO, 0);
    char ab[16];
    snprintf(ab, sizeof(ab), auto_min ? "%d MIN" : "OFF", auto_min);
    lv_obj_t *autob = deck_button(arow, ab, g_font.disp_s);
    lv_obj_set_style_min_height(autob, 40, 0);
    lv_obj_set_style_pad_ver(autob, 6, 0);
    lv_obj_set_width(autob, 110);
    lv_obj_align(autob, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(autob, auto_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *note = deck_label(left, g_font.mono_s, g_pal.dim,
                                "Markdown notes sync both ways; attachments stay in Obsidian.\n"
                                "Auto sync runs when NOTES opens if the last one is older than this.");
    lv_obj_set_width(note, LV_PCT(100));
    lv_obj_set_style_pad_top(note, 8, 0);

    lv_obj_t *right = deck_panel(cols, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right, 20, 0);
    lv_obj_set_style_pad_row(right, 10, 0);
    lv_obj_t *srow = deck_box(right);
    lv_obj_set_width(srow, LV_PCT(100));
    lv_obj_set_height(srow, 50);
    s_n.sync_status = deck_label(srow, g_font.disp_m, g_pal.dim, "");
    lv_obj_align(s_n.sync_status, LV_ALIGN_LEFT_MID, 0, 0);
    s_n.sync_btn = deck_button(srow, "SYNC NOW", g_font.disp_s);
    lv_obj_set_style_min_height(s_n.sync_btn, 46, 0);
    lv_obj_align(s_n.sync_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_n.sync_btn, sync_now_clicked, LV_EVENT_CLICKED, NULL);
    /* Walk the whole database again: picks up anything a past run missed. */
    lv_obj_t *rescan = deck_button(srow, "RESCAN", g_font.disp_s);
    lv_obj_set_style_min_height(rescan, 46, 0);
    lv_obj_align_to(rescan, s_n.sync_btn, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_obj_add_event_cb(rescan, rescan_clicked, LV_EVENT_CLICKED, NULL);
    deck_section(right, "LOG");
    lv_obj_t *logbox = deck_box(right);
    lv_obj_set_width(logbox, LV_PCT(100));
    lv_obj_set_flex_grow(logbox, 1);
    lv_obj_add_flag(logbox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(logbox, LV_DIR_VER);
    s_n.sync_log = deck_label(logbox, g_font.mono_s, g_pal.text, "");
    lv_obj_set_width(s_n.sync_log, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_n.sync_log, 4, 0);

    deck_label(root, g_font.mono_s, g_pal.dim, "[ESC] BACK TO THE VAULT    [S] SYNC NOW");
    sync_refresh(NULL);
    lv_group_focus_obj(s_n.sync_btn);
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
    bool vault      = notes_vault_mode();
    int files = 0, folders = 0;
    for (int i = 0; i < s_n.count; i++) {
        if (s_n.notes[i].is_dir) folders++;
        else files++;
    }

    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_t *t = deck_label(head, g_font.disp_s, g_pal.accent, vault ? "// VAULT" : "// DATASTORE");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    char info[400], where[160];
    if (dir == NULL) {
        snprintf(where, sizeof(where), "NO STORAGE");
    } else {
        snprintf(where, sizeof(where), "%s/%s", vault ? "vault" : "notes", notes_subdir());
    }
    char fold[40] = "";
    if (folders) snprintf(fold, sizeof(fold), "%d FOLDER%s  ::  ", folders, folders == 1 ? "" : "S");
    snprintf(info, sizeof(info), "%s  ::  %s%d FILE%s  ::  %s", where, fold, files, files == 1 ? "" : "S",
             hal_storage_is_sd() ? "SD" : "INTERNAL FLASH");
    lv_obj_t *path = deck_label(head, g_font.mono_s, g_pal.dim, info);
    lv_label_set_long_mode(path, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(path, 640);
    lv_obj_align(path, LV_ALIGN_LEFT_MID, 150, 0);
    s_n.sync_open = false;
    lv_obj_t *nb = tool_button(head, "+ NEW", new_clicked, NULL);
    lv_obj_align(nb, LV_ALIGN_RIGHT_MID, 0, 0);
    /* Which tree: the deck's own notes, or the synced Obsidian vault. */
    lv_obj_t *src = tool_button(head, vault ? "VAULT" : "LOCAL", source_clicked, NULL);
    lv_obj_align(src, LV_ALIGN_RIGHT_MID, -116, 0);
    if (vault) {
        lv_obj_t *sb = tool_button(head, deck_sync_state() == SYNC_RUNNING ? "SYNCING" : "SYNC", sync_clicked, NULL);
        lv_obj_align(sb, LV_ALIGN_RIGHT_MID, -228, 0);
        lv_obj_set_width(path, 520);
    }

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
    } else if (s_n.count == 0 && vault && notes_subdir()[0] == '\0') {
        deck_label(list, g_font.mono_m, g_pal.dim, "Empty vault. SYNC pulls your notes in; LOCAL switches back.");
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

        lv_obj_t *nm = deck_icon_text(row, n->is_dir ? ICON_FOLDER : ICON_MARKDOWN, n->name, g_font.mono_l,
                                      n->is_dir ? g_pal.accent : g_pal.text);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 0, 0);

        if (n->is_dir) {
            lv_obj_t *m = deck_label(row, g_font.mono_s, g_pal.dim, "FOLDER");
            lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);
            if (focus == NULL || (focus_name && strcmp(focus_name, n->name) == 0)) focus = row;
            continue;
        }

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
               notes_subdir()[0] ? "[ENTER] OPEN    [BACKSPACE] UP    [CTRL+N] NEW    [CTRL+R] RENAME    [DEL] DELETE    [ESC] DECK"
                                 : "[ENTER] OPEN    [CTRL+N] NEW    [CTRL+R] RENAME    [DEL] DELETE    [ESC] DECK");

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
    if (s_n.sync_open) {
        if (k->code == HID_ESC) {
            show_browser(NULL);
            return true;
        }
        if (k->code == HID_S && !(k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) {
            sync_now_clicked(NULL);
            return true;
        }
        return false;
    }

    bool ctrl = (k->mods & DECK_MOD_CTRL) != 0;
    if (ctrl && k->code == HID_N) {
        new_note();
        return true;
    }
    if (k->code == HID_BACKSPACE && !ctrl && notes_subdir()[0]) {
        go_up();
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
    /* A stale vault syncs on its own when AUTO SYNC is set. */
    int auto_min = (int)hal_cfg_get_i32(SYNC_KEY_AUTO, 0);
    if (notes_vault_mode() && auto_min > 0 && deck_sync_configured() && deck_sync_state() != SYNC_RUNNING) {
        long age = deck_sync_age();
        if (age < 0 || age > (long)auto_min * 60) deck_sync_start();
    }
    s_n.sync_seen = deck_sync_state();
    s_n.sync_timer = lv_timer_create(browser_sync_tick, 500, NULL);
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
    if (s_n.sync_timer) lv_timer_delete(s_n.sync_timer);
    memset(&s_n, 0, sizeof(s_n));
}

static deck_app_t s_app = {
    .name     = "NOTES",
    .tagline  = "MARKDOWN I/O",
    .icon     = ICON_MARKDOWN,
    .eta      = NULL,
    .hotkey   = HID_LETTER('N'),
    .on_start = start,
    .on_exit  = notes_exit,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_notes(void) { return &s_app; }
