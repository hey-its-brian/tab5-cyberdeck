/*
 * FILES module: browse the deck's storage (the SD card, or internal flash
 * without one), preview text and markdown, play MP3s, delete files and
 * folders; and the LAN file portal on its own tab. The portal keeps running
 * after you leave; the status bar shows LINK while it is up.
 *
 * Keys: Up/Down move, Enter opens a folder (or plays an MP3 already
 * selected), Backspace goes up, Delete or D deletes, P switches between
 * PREVIEW and PORTAL, Space engages / disengages the portal on its tab.
 */
#include "app_files.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "deck_audio.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_input.h"
#include "deck_md.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_portal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define MAX_ENTRIES 400
#define PREVIEW_MAX (12 * 1024)
#define LOG_LINES 6

typedef enum { KIND_DIR, KIND_MD, KIND_TEXT, KIND_MP3, KIND_OTHER } kind_t;

typedef struct {
    char name[128];
    bool dir;
    uint32_t size;
    time_t mtime;
} entry_t;

static struct {
    /* browser */
    lv_obj_t *path, *space, *list, *empty;
    lv_obj_t *rows[MAX_ENTRIES];
    int sel;           /* selected entry, -1 none */
    uint32_t sel_tick; /* when it was selected: a tap focuses then clicks, so
                        * a click right after its own focus only selects */
    /* right side */
    lv_obj_t *tab_preview, *tab_portal;
    lv_obj_t *preview, *portal;
    lv_obj_t *p_title, *p_meta, *p_play, *p_delete, *p_body;
    /* portal */
    lv_obj_t *state, *toggle, *on_box, *off_box, *off_msg, *url_host, *url_ip, *pin, *qr, *idle, *log;
    char ip_shown[16];
    bool portal_tab;
    lv_timer_t *timer;
} s_ui;

static char s_cwd[256];     /* full path of the folder shown */
static entry_t *s_entries;  /* MAX_ENTRIES, allocated once */
static int s_count;
static char s_del_path[400];
static char s_keep[128]; /* entry to reselect after the next rebuild */

/* Portal events survive leaving the screen, so the log shows recent history. */
static char s_log[LOG_LINES][80];
static int s_log_n;

/* ---- Helpers ------------------------------------------------------------- */

static const char *root_path(void) { return hal_storage_root() ? hal_storage_root() : ""; }
static bool at_root(void) { return strcmp(s_cwd, root_path()) == 0; }

static bool has_ext(const char *name, const char *const *exts)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;
    for (; *exts; exts++) {
        if (strcasecmp(dot + 1, *exts) == 0) return true;
    }
    return false;
}

static kind_t kind_of(const entry_t *e)
{
    static const char *const md[]   = {"md", "markdown", NULL};
    static const char *const text[] = {"txt", "log", "json", "csv", "ini", "cfg", "conf", "yaml", "yml", "xml",
                                       "html", "htm", "c", "h", "cpp", "py", "js", "sh", "toml", NULL};
    static const char *const mp3[]  = {"mp3", NULL};
    if (e->dir) return KIND_DIR;
    if (has_ext(e->name, md)) return KIND_MD;
    if (has_ext(e->name, text)) return KIND_TEXT;
    if (has_ext(e->name, mp3)) return KIND_MP3;
    return KIND_OTHER;
}

static void human(char *b, size_t n, uint64_t v)
{
    if (v < 1024) {
        snprintf(b, n, "%u B", (unsigned)v);
    } else if (v < 1024 * 1024) {
        snprintf(b, n, "%.1f KB", v / 1024.0);
    } else if (v < 1024ull * 1024 * 1024) {
        snprintf(b, n, "%.1f MB", v / 1048576.0);
    } else {
        snprintf(b, n, "%.2f GB", v / 1073741824.0);
    }
}

static void entry_path(const entry_t *e, char *out, size_t n) { snprintf(out, n, "%s/%s", s_cwd, e->name); }

/* Text-ish if the start has no NULs and is mostly printable. */
static bool looks_text(const char *b, size_t n)
{
    size_t bad = 0;
    for (size_t i = 0; i < n && i < 512; i++) {
        unsigned char c = (unsigned char)b[i];
        if (c == 0) return false;
        if (c < 0x09 || (c > 0x0D && c < 0x20)) bad++;
    }
    return bad * 20 <= (n < 512 ? n : 512); /* at most 5% control bytes */
}

/* ---- Listing ------------------------------------------------------------- */

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan(void)
{
    s_count = 0;
    DIR *d  = opendir(s_cwd);
    if (d == NULL) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_count < MAX_ENTRIES) {
        if (de->d_name[0] == '.') continue;
        /* The SSH folder (known_hosts) stays private, as in the portal. */
        if (at_root() && strcasecmp(de->d_name, "ssh") == 0) continue;
        entry_t *e = &s_entries[s_count];
        size_t nl  = strlen(de->d_name);
        if (nl >= sizeof(e->name)) continue; /* a cut-off name would point at the wrong file */
        memcpy(e->name, de->d_name, nl + 1);
        char p[400];
        entry_path(e, p, sizeof(p));
        struct stat st;
        if (stat(p, &st) != 0) continue;
        e->dir   = S_ISDIR(st.st_mode);
        e->size  = (uint32_t)st.st_size;
        e->mtime = st.st_mtime;
        s_count++;
    }
    closedir(d);
    qsort(s_entries, (size_t)s_count, sizeof(entry_t), cmp_entry);
}

static void show_preview(int i);
static void build_list(void);

static void go_up(void)
{
    if (at_root()) return;
    char *slash = strrchr(s_cwd, '/');
    if (slash) *slash = '\0';
    build_list();
}

static void enter_dir(const entry_t *e)
{
    size_t l = strlen(s_cwd), n = strlen(e->name);
    if (l + 1 + n >= sizeof(s_cwd)) return; /* too deep to open */
    s_cwd[l] = '/';
    memcpy(s_cwd + l + 1, e->name, n + 1);
    build_list();
}

/* Index of entry i among this folder's MP3s, which is the player's queue order. */
static int track_index(int i)
{
    int t = 0;
    for (int k = 0; k < i; k++) {
        if (kind_of(&s_entries[k]) == KIND_MP3) t++;
    }
    return t;
}

static void play_selected(void)
{
    if (s_ui.sel < 0 || kind_of(&s_entries[s_ui.sel]) != KIND_MP3) return;
    player_open_dir(s_cwd, track_index(s_ui.sel));
}

static void row_focused(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i >= 0) {
        show_preview((int)i);
        s_ui.sel_tick = lv_tick_get();
    }
}

static void row_clicked(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i < 0) {
        go_up();
        return;
    }
    entry_t *x = &s_entries[i];
    if (x->dir) {
        enter_dir(x);
    } else if (s_ui.sel == (int)i && kind_of(x) == KIND_MP3 && lv_tick_elaps(s_ui.sel_tick) > 400) {
        play_selected(); /* second tap, or Enter on the selected track */
    } else {
        show_preview((int)i);
    }
}

static lv_obj_t *add_row(const char *icon, const char *name, const char *right, intptr_t ud)
{
    lv_obj_t *row = deck_panel(s_ui.list, DECK_CUT_BR, 10);
    lv_obj_set_size(row, LV_PCT(100), 48);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)ud);
    lv_obj_add_event_cb(row, row_focused, LV_EVENT_FOCUSED, (void *)ud);
    lv_group_add_obj(deck_input_group(), row);
    lv_obj_t *l = deck_icon_text(row, icon, name, g_font.mono_m, g_pal.text);
    lv_obj_set_width(l, LV_PCT(72));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    if (right) {
        lv_obj_t *r = deck_label(row, g_font.mono_s, g_pal.dim, right);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return row;
}

static void build_list(void)
{
    lv_obj_clean(s_ui.list);
    memset(s_ui.rows, 0, sizeof(s_ui.rows));
    s_ui.sel = -1;
    scan();

    char shown[300];
    snprintf(shown, sizeof(shown), "%s:%s", hal_storage_is_sd() ? "SD" : "FLASH",
             at_root() ? "/" : s_cwd + strlen(root_path()));
    lv_label_set_text(s_ui.path, shown);
    if (hal_storage_is_sd()) {
        char f[24], t[24];
        human(f, sizeof(f), hal_sd_free_bytes());
        human(t, sizeof(t), hal_sd_total_bytes());
        lv_label_set_text_fmt(s_ui.space, "%s FREE OF %s", f, t);
    } else {
        lv_label_set_text(s_ui.space, "INTERNAL STORAGE");
    }

    lv_obj_t *first = NULL;
    if (!at_root()) first = add_row(ICON_CHEVRON_LEFT, "..", NULL, -1);
    for (int i = 0; i < s_count; i++) {
        entry_t *x = &s_entries[i];
        char sz[24] = "";
        if (!x->dir) human(sz, sizeof(sz), x->size);
        kind_t k     = kind_of(x);
        const char *icon = k == KIND_DIR ? ICON_FOLDER : k == KIND_MP3 ? ICON_NOTE : k == KIND_MD ? ICON_MARKDOWN : ICON_SD;
        s_ui.rows[i] = add_row(icon, x->name, x->dir ? NULL : sz, i);
        if (!first) first = s_ui.rows[i];
    }
    if (s_count == 0 && at_root()) {
        lv_obj_remove_flag(s_ui.empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.empty, LV_OBJ_FLAG_HIDDEN);
    }
    show_preview(-1);
    for (int i = 0; s_keep[0] && i < s_count; i++) {
        if (strcmp(s_entries[i].name, s_keep) == 0) first = s_ui.rows[i];
    }
    s_keep[0] = '\0';
    if (first) lv_group_focus_obj(first); /* focusing a file row previews it */
}

/* ---- Preview ------------------------------------------------------------- */

/* A wrapped line of text in the preview body. */
static void body_note(const char *text, lv_color_t color)
{
    lv_obj_t *l = deck_label(s_ui.p_body, g_font.mono_m, color, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
}

static void preview_text(const char *path, bool markdown, uint32_t size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        body_note("Could not open the file.", g_pal.warn);
        return;
    }
    char *buf = malloc(PREVIEW_MAX + 64);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    size_t n = fread(buf, 1, PREVIEW_MAX, f);
    fclose(f);
    buf[n] = '\0';
    if (!looks_text(buf, n)) {
        body_note("Binary data, no preview.", g_pal.dim);
        free(buf);
        return;
    }
    if (size > n) strcat(buf, "\n\n[first 12 KB shown]");
    if (markdown) {
        deck_md_render(s_ui.p_body, buf, strlen(buf));
    } else {
        lv_obj_t *l = deck_label(s_ui.p_body, g_font.mono_s, g_pal.text, buf);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
    free(buf);
}

static void show_preview(int i)
{
    if (s_ui.p_body == NULL) return;
    s_ui.sel = i;
    for (int k = 0; k < s_count; k++) {
        if (s_ui.rows[k]) deck_panel_set_outline(s_ui.rows[k], k == i ? g_pal.accent : g_pal.line);
    }
    lv_obj_clean(s_ui.p_body);
    lv_obj_scroll_to_y(s_ui.p_body, 0, LV_ANIM_OFF);
    lv_obj_add_flag(s_ui.p_play, LV_OBJ_FLAG_HIDDEN);
    if (i < 0) {
        lv_label_set_text(s_ui.p_title, "NOTHING SELECTED");
        lv_label_set_text(s_ui.p_meta, "");
        lv_obj_add_flag(s_ui.p_delete, LV_OBJ_FLAG_HIDDEN);
        body_note("Pick a file to preview it here.", g_pal.dim);
        return;
    }
    entry_t *e = &s_entries[i];
    kind_t k   = kind_of(e);
    lv_obj_remove_flag(s_ui.p_delete, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_ui.p_title, e->name);

    char when[32] = "";
    struct tm tm;
    if (e->mtime > 0 && localtime_r(&e->mtime, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    char sz[24];
    human(sz, sizeof(sz), e->size);
    static const char *const kinds[] = {"FOLDER", "MARKDOWN", "TEXT", "MP3 AUDIO", "FILE"};
    if (e->dir) {
        lv_label_set_text_fmt(s_ui.p_meta, "%s  //  %s", kinds[k], when);
        body_note("Folder. ENTER opens it; DELETE removes it and everything in it.", g_pal.dim);
        return;
    }
    lv_label_set_text_fmt(s_ui.p_meta, "%s  //  %s  //  %s", kinds[k], sz, when);

    char path[400];
    entry_path(e, path, sizeof(path));
    if (k == KIND_MP3) {
        lv_obj_remove_flag(s_ui.p_play, LV_OBJ_FLAG_HIDDEN);
        body_note("PLAY (or ENTER again) starts this folder in PLAYER from this track.", g_pal.dim);
    } else if (k == KIND_MD || k == KIND_TEXT || k == KIND_OTHER) {
        preview_text(path, k == KIND_MD, e->size);
    }
}

static void play_clicked(lv_event_t *e)
{
    (void)e;
    play_selected();
}

/* ---- Delete -------------------------------------------------------------- */

static bool remove_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    if (depth > 8) return false;
    DIR *d = opendir(path);
    if (d == NULL) return false;
    struct dirent *de;
    char child[512];
    bool ok = true;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        ok = remove_tree(child, depth + 1) && ok;
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

/* The player holds its track open, and FAT does not protect open files:
 * stop it before deleting the track, or a folder that contains it. */
static void release_player(const char *path)
{
    if (player_state() == PLAYER_STOPPED) return;
    char cur[400];
    snprintf(cur, sizeof(cur), "%s/%s", player_dir(), player_file(player_index()));
    size_t n = strlen(path);
    if (strcmp(cur, path) != 0 && !(strncmp(cur, path, n) == 0 && cur[n] == '/')) return;
    player_stop();
    for (int i = 0; i < 50 && player_state() != PLAYER_STOPPED; i++) lv_delay_ms(10);
}

static void delete_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (!yes || s_ui.list == NULL) return;
    if (portal_busy()) {
        deck_modal_confirm("DELETE", "A portal transfer is running. Try again when it finishes.", "OK", NULL, NULL);
        return;
    }
    release_player(s_del_path);
    if (!remove_tree(s_del_path, 0)) {
        deck_modal_confirm("DELETE", "Could not delete everything (a file may be in use).", "OK", NULL, NULL);
    }
    build_list();
}

static void ask_delete(void)
{
    if (s_ui.sel < 0 || s_ui.sel >= s_count) return;
    entry_t *e = &s_entries[s_ui.sel];
    entry_path(e, s_del_path, sizeof(s_del_path));
    char msg[220];
    snprintf(msg, sizeof(msg), "Delete %s%.120s%s? This can't be undone.", e->dir ? "the folder \"" : "\"", e->name,
             e->dir ? "\" and everything in it" : "\"");
    deck_modal_confirm_danger("DELETE", msg, "DELETE", delete_confirmed, NULL);
}

static void delete_clicked(lv_event_t *e)
{
    (void)e;
    ask_delete();
}

/* ---- Portal tab ---------------------------------------------------------- */

static void log_push(const portal_event_t *e)
{
    if (s_log_n == LOG_LINES) {
        memmove(s_log[0], s_log[1], sizeof(s_log[0]) * (LOG_LINES - 1));
        s_log_n--;
    }
    snprintf(s_log[s_log_n++], sizeof(s_log[0]), "%s %s", e->error ? "!" : ">", e->text);
}

static void render_log(void)
{
    static char buf[LOG_LINES * 80];
    size_t k = 0;
    buf[0]   = '\0';
    for (int i = 0; i < s_log_n; i++) k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%s\n", s_log[i]);
    lv_label_set_text(s_ui.log, s_log_n ? buf : "> no activity yet");
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    static uint32_t s_cursor;
    portal_event_t ev[8];
    int n;
    bool changed = false, files_changed = false;
    while ((n = portal_events(ev, 8, &s_cursor)) > 0) {
        for (int i = 0; i < n; i++) {
            log_push(&ev[i]);
            /* An upload, delete or rename from the browser changes the list. */
            if (strncmp(ev[i].text, "received", 8) == 0 || strncmp(ev[i].text, "deleted", 7) == 0 ||
                strncmp(ev[i].text, "renamed", 7) == 0 || strncmp(ev[i].text, "new folder", 10) == 0) {
                files_changed = true;
            }
        }
        changed = true;
    }
    if (changed) render_log();
    if (files_changed && !deck_modal_active()) {
        if (s_ui.sel >= 0) snprintf(s_keep, sizeof(s_keep), "%s", s_entries[s_ui.sel].name);
        build_list(); /* keep the selection where it was */
    }

    bool on = portal_running();
    lv_label_set_text(s_ui.state, on ? "LINK ACTIVE" : "LINK DOWN");
    lv_obj_set_style_text_color(s_ui.state, on ? g_pal.ok : g_pal.dim, 0);
    lv_label_set_text(lv_obj_get_child_by_type(s_ui.toggle, 0, &lv_label_class), on ? "DISENGAGE" : "ENGAGE");
    lv_label_set_text(lv_obj_get_child_by_type(s_ui.tab_portal, 0, &lv_label_class), on ? "PORTAL  [LINK]" : "PORTAL");

    if (on) {
        lv_obj_remove_flag(s_ui.on_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.off_box, LV_OBJ_FLAG_HIDDEN);
        if (strcmp(s_ui.ip_shown, net_ip()) != 0) {
            snprintf(s_ui.ip_shown, sizeof(s_ui.ip_shown), "%s", net_ip());
            char url[48];
            snprintf(url, sizeof(url), "http://%s", net_ip());
            lv_label_set_text(s_ui.url_ip, url);
            lv_qrcode_update(s_ui.qr, url, (uint32_t)strlen(url));
        }
        lv_label_set_text_fmt(s_ui.url_host, "http://%s", portal_host());
        lv_label_set_text(s_ui.pin, portal_pin());
        uint32_t idle = portal_idle_s();
        if (portal_busy()) {
            lv_label_set_text(s_ui.idle, "TRANSFER IN PROGRESS");
        } else {
            uint32_t left = idle >= PORTAL_IDLE_OFF_S ? 0 : (PORTAL_IDLE_OFF_S - idle + 59) / 60;
            lv_label_set_text_fmt(s_ui.idle, "AUTO-OFF IN %lu MIN WITHOUT ACTIVITY", (unsigned long)left);
        }
    } else {
        lv_obj_add_flag(s_ui.on_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.off_box, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui.idle, "");
        s_ui.ip_shown[0] = '\0';
        bool online      = net_state() == NET_CONNECTED;
        lv_label_set_text(s_ui.off_msg, online ? "Share this storage with any browser on your network.\n"
                                                 "ENGAGE gives you the address and a one-time PIN."
                                               : "No network. Join Wi-Fi in SYSTEM first.");
        lv_obj_set_style_text_color(s_ui.off_msg, online ? g_pal.text : g_pal.warn, 0);
    }
}

static void toggle_portal(void)
{
    if (portal_running()) {
        portal_stop();
    } else if (!portal_start()) {
        portal_event_t e = {.error = true};
        snprintf(e.text, sizeof(e.text), "%s", net_state() == NET_CONNECTED ? "could not start (no storage?)"
                                                                            : "no network: join Wi-Fi in SYSTEM");
        log_push(&e);
        render_log();
    }
    refresh(NULL);
}

static void toggle_clicked(lv_event_t *e)
{
    (void)e;
    toggle_portal();
}

static void set_tab(bool portal)
{
    s_ui.portal_tab = portal;
    if (portal) {
        lv_obj_add_flag(s_ui.preview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ui.preview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.portal, LV_OBJ_FLAG_HIDDEN);
    }
    deck_panel_set_outline(s_ui.tab_preview, portal ? g_pal.line : g_pal.accent);
    deck_panel_set_outline(s_ui.tab_portal, portal ? g_pal.accent : g_pal.line);
}

static void tab_clicked(lv_event_t *e) { set_tab((intptr_t)lv_event_get_user_data(e) != 0); }

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = deck_button(parent, text, NULL);
    lv_obj_set_style_min_height(b, 46, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static void build_portal(lv_obj_t *parent)
{
    lv_obj_t *head = deck_box(parent);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    s_ui.state = deck_label(head, g_font.disp_m, g_pal.dim, "");
    lv_obj_align(s_ui.state, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.toggle = small_button(head, "ENGAGE", toggle_clicked, 0);
    lv_obj_align(s_ui.toggle, LV_ALIGN_RIGHT_MID, 0, 0);

    s_ui.on_box = deck_box(parent);
    lv_obj_set_size(s_ui.on_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui.on_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_ui.on_box, 20, 0);
    lv_obj_t *txt = deck_box(s_ui.on_box);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 4, 0);
    deck_label(txt, g_font.mono_s, g_pal.dim, "OPEN IN A BROWSER ON THE SAME NETWORK");
    s_ui.url_host = deck_label(txt, g_font.disp_m, g_pal.accent, "");
    s_ui.url_ip   = deck_label(txt, g_font.mono_l, g_pal.text, "");
    lv_obj_t *pl  = deck_label(txt, g_font.mono_s, g_pal.dim, "PIN");
    lv_obj_set_style_margin_top(pl, 10, 0);
    s_ui.pin = deck_label(txt, g_font.disp_xl, g_pal.accent2, "");
    lv_obj_set_style_text_letter_space(s_ui.pin, 8, 0);
    s_ui.qr = lv_qrcode_create(s_ui.on_box);
    lv_qrcode_set_size(s_ui.qr, 170);
    lv_qrcode_set_dark_color(s_ui.qr, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr, lv_color_white());
    lv_qrcode_set_quiet_zone(s_ui.qr, true);

    s_ui.off_box = deck_box(parent);
    lv_obj_set_size(s_ui.off_box, LV_PCT(100), LV_SIZE_CONTENT);
    s_ui.off_msg = deck_label(s_ui.off_box, g_font.mono_m, g_pal.text, "");
    lv_obj_set_width(s_ui.off_msg, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_ui.off_msg, 6, 0);

    s_ui.idle = deck_label(parent, g_font.mono_s, g_pal.dim, "");
    deck_section(parent, "LINK LOG");
    s_ui.log = deck_label(parent, g_font.mono_s, g_pal.text, "");
    lv_obj_set_width(s_ui.log, LV_PCT(100));
    lv_label_set_long_mode(s_ui.log, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_line_space(s_ui.log, 4, 0);
}

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.sel = -1;
    if (s_entries == NULL) s_entries = calloc(MAX_ENTRIES, sizeof(entry_t));
    if (s_cwd[0] == '\0' || strncmp(s_cwd, root_path(), strlen(root_path())) != 0) {
        snprintf(s_cwd, sizeof(s_cwd), "%s", root_path());
    }

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_column(root, 20, 0);

    /* Left: browser */
    lv_obj_t *left = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(left, 560, LV_PCT(100));
    deck_panel_set_tab(left, true);
    lv_obj_set_style_pad_all(left, 22, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);
    deck_section(left, "STORAGE");
    lv_obj_t *ph = deck_box(left);
    lv_obj_set_size(ph, LV_PCT(100), LV_SIZE_CONTENT);
    s_ui.path = deck_label(ph, g_font.mono_s, g_pal.accent, "");
    lv_obj_set_width(s_ui.path, LV_PCT(58));
    lv_label_set_long_mode(s_ui.path, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_ui.path, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.space = deck_label(ph, g_font.mono_s, g_pal.dim, "");
    lv_obj_align(s_ui.space, LV_ALIGN_RIGHT_MID, 0, 0);
    s_ui.list = deck_box(left);
    lv_obj_set_width(s_ui.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, 6, 0);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    s_ui.empty = deck_label(left, g_font.mono_m, g_pal.dim, "Empty. Drop files in with the PORTAL tab.");
    lv_obj_set_width(s_ui.empty, LV_PCT(100));
    lv_label_set_long_mode(s_ui.empty, LV_LABEL_LONG_MODE_WRAP);

    /* Right: tabs */
    lv_obj_t *right = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_all(right, 22, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    lv_obj_t *tabs = deck_box(right);
    lv_obj_set_size(tabs, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 12, 0);
    s_ui.tab_preview = small_button(tabs, "PREVIEW", tab_clicked, 0);
    s_ui.tab_portal  = small_button(tabs, "PORTAL", tab_clicked, 1);

    /* PREVIEW */
    s_ui.preview = deck_box(right);
    lv_obj_set_width(s_ui.preview, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.preview, 1);
    lv_obj_set_flex_flow(s_ui.preview, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.preview, 8, 0);
    s_ui.p_title = deck_label(s_ui.preview, g_font.disp_m, g_pal.accent, "");
    lv_obj_set_width(s_ui.p_title, LV_PCT(100));
    lv_label_set_long_mode(s_ui.p_title, LV_LABEL_LONG_MODE_DOTS);
    s_ui.p_meta = deck_label(s_ui.preview, g_font.mono_s, g_pal.dim, "");
    lv_obj_t *acts = deck_box(s_ui.preview);
    lv_obj_set_size(acts, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(acts, 12, 0);
    s_ui.p_play   = small_button(acts, "PLAY", play_clicked, 0);
    s_ui.p_delete = small_button(acts, "DELETE", delete_clicked, 0);
    s_ui.p_body   = deck_box(s_ui.preview);
    lv_obj_set_width(s_ui.p_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.p_body, 1);
    lv_obj_set_flex_flow(s_ui.p_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_ui.p_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.p_body, LV_DIR_VER);

    /* PORTAL */
    s_ui.portal = deck_box(right);
    lv_obj_set_width(s_ui.portal, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.portal, 1);
    lv_obj_set_flex_flow(s_ui.portal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.portal, 10, 0);
    build_portal(s_ui.portal);

    set_tab(portal_running()); /* come back to the portal while it is up */
    render_log();
    refresh(NULL);
    build_list();
    s_ui.timer = lv_timer_create(refresh, 500, NULL);
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    memset(&s_ui, 0, sizeof(s_ui));
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    switch (k->code) {
        case HID_BACKSPACE: go_up(); return true;
        case HID_DELETE: ask_delete(); return true;
        case HID_SPACE:
            if (s_ui.portal_tab) {
                toggle_portal();
                return true;
            }
            return false;
        default: break;
    }
    switch (k->key) {
        case 'd':
        case 'D': ask_delete(); return true;
        case 'p':
        case 'P': set_tab(!s_ui.portal_tab); return true;
        default: return false;
    }
}

static deck_app_t s_app = {
    .name     = "FILES",
    .tagline  = "STORAGE + UPLINK",
    .icon     = ICON_FOLDER,
    .eta      = NULL,
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_files(void) { return &s_app; }
