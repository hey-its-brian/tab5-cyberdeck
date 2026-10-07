/*
 * PLAYER module: browse /music (folders and .mp3 files) on the left, now
 * playing on the right with a spectrum, progress, transport and volume.
 * Music keeps playing after you leave; Alt+P pauses from anywhere.
 *
 * Keys: Enter plays / opens, Space pause, Left/Right seek 10 s, [ ] prev /
 * next, - = volume, S shuffle, R repeat, Backspace up a folder.
 */
#include "app_player.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "deck_audio.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_input.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define MAX_ROWS 200
#define VIS_H 170

typedef struct {
    char name[96];
    bool dir;
    int track; /* index among the folder's mp3s, or -1 */
} entry_t;

static struct {
    lv_obj_t *path;
    lv_obj_t *list;
    lv_obj_t *empty;
    lv_obj_t *title, *artist;
    lv_obj_t *bars[PLAYER_BANDS];
    lv_obj_t *progress, *time;
    lv_obj_t *play, *shuffle, *repeat;
    lv_obj_t *vol, *vol_val;
    lv_obj_t *out;
    lv_obj_t *rows[MAX_ROWS];
    lv_timer_t *timer;
    int shown_index;
    char shown_dir[192];
} s_ui;

static char s_cwd[192];
static entry_t *s_entries; /* MAX_ROWS, allocated once */
static int s_entry_n;

/* ---- Library ------------------------------------------------------------- */

static void music_root(char *out, size_t n)
{
    const char *root = hal_storage_root();
    snprintf(out, n, "%s/music", root ? root : "");
}

static bool at_root(void)
{
    char r[192];
    music_root(r, sizeof(r));
    return strcmp(s_cwd, r) == 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan(void)
{
    s_entry_n = 0;
    DIR *d    = opendir(s_cwd);
    if (d == NULL) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_entry_n < MAX_ROWS) {
        if (e->d_name[0] == '.') continue;
        char p[512];
        snprintf(p, sizeof(p), "%s/%s", s_cwd, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        size_t l = strlen(e->d_name);
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && !(l > 4 && strcasecmp(e->d_name + l - 4, ".mp3") == 0)) continue;
        entry_t *x = &s_entries[s_entry_n++];
        snprintf(x->name, sizeof(x->name), "%s", e->d_name);
        x->dir   = dir;
        x->track = -1;
    }
    closedir(d);
    qsort(s_entries, (size_t)s_entry_n, sizeof(entry_t), cmp_entry);
    /* Track numbers match the player's queue order (mp3s sorted by name). */
    int t = 0;
    for (int i = 0; i < s_entry_n; i++) {
        if (!s_entries[i].dir) s_entries[i].track = t++;
    }
}

static void build_list(void);

static void row_clicked(lv_event_t *e)
{
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i < 0) { /* ".." */
        char *slash = strrchr(s_cwd, '/');
        if (slash && !at_root()) *slash = '\0';
        build_list();
        return;
    }
    entry_t *x = &s_entries[i];
    if (x->dir) {
        size_t l = strlen(s_cwd);
        if (l + 1 + strlen(x->name) >= sizeof(s_cwd)) return; /* too deep to open */
        s_cwd[l] = '/';
        memcpy(s_cwd + l + 1, x->name, strlen(x->name) + 1);
        build_list();
    } else if (strcmp(player_dir(), s_cwd) == 0 && player_index() == x->track) {
        player_toggle();
    } else {
        player_open_dir(s_cwd, x->track);
    }
}

static lv_obj_t *add_row(const char *icon, const char *text, intptr_t ud)
{
    lv_obj_t *row = deck_panel(s_ui.list, DECK_CUT_BR, 10);
    lv_obj_set_size(row, LV_PCT(100), 52);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)ud);
    lv_group_add_obj(deck_input_group(), row);
    lv_obj_t *l = deck_icon_text(row, icon, text, g_font.mono_m, g_pal.text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

static void build_list(void)
{
    lv_obj_clean(s_ui.list);
    memset(s_ui.rows, 0, sizeof(s_ui.rows));
    scan();
    char shown[208];
    char r[192];
    music_root(r, sizeof(r));
    snprintf(shown, sizeof(shown), "/music%s", s_cwd + strlen(r));
    lv_label_set_text(s_ui.path, shown);

    lv_obj_t *first = NULL;
    if (!at_root()) first = add_row(ICON_CHEVRON_LEFT, "..", -1);
    for (int i = 0; i < s_entry_n; i++) {
        entry_t *x = &s_entries[i];
        char name[sizeof(x->name)];
        memcpy(name, x->name, sizeof(name));
        if (!x->dir) {
            char *dot = strrchr(name, '.');
            if (dot) *dot = '\0';
        }
        s_ui.rows[i] = add_row(x->dir ? ICON_FOLDER : ICON_NOTE, name, i);
        if (!first) first = s_ui.rows[i];
    }
    bool none = s_entry_n == 0 && at_root();
    if (none) {
        lv_obj_remove_flag(s_ui.empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui.empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (first) lv_group_focus_obj(first);
    s_ui.shown_index = -2; /* re-mark the playing row */
}

/* ---- Now playing --------------------------------------------------------- */

static void fmt_time(char *b, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void set_icon(lv_obj_t *btn, const char *icon, lv_color_t c)
{
    lv_obj_t *l = lv_obj_get_child_by_type(btn, 0, &lv_label_class);
    lv_label_set_text(l, icon);
    lv_obj_set_style_text_color(l, c, 0);
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    player_state_t st = player_state();
    bool loaded       = st != PLAYER_STOPPED;
    lv_label_set_text(s_ui.title, loaded ? player_title() : "NO SIGNAL");
    lv_label_set_text(s_ui.artist, loaded ? player_artist() : (player_error()[0] ? player_error() : "pick a track"));

    uint8_t lv[PLAYER_BANDS];
    player_levels(lv);
    for (int i = 0; i < PLAYER_BANDS; i++) {
        int h = st == PLAYER_PLAYING ? 4 + lv[i] * (VIS_H - 4) / 255 : 4;
        lv_obj_set_height(s_ui.bars[i], h);
    }

    uint32_t pos = player_pos_ms(), dur = player_dur_ms();
    lv_bar_set_value(s_ui.progress, dur ? (int32_t)((uint64_t)pos * 1000 / dur) : 0, LV_ANIM_OFF);
    char a[16], b[16], tb[40];
    fmt_time(a, sizeof(a), pos);
    fmt_time(b, sizeof(b), dur);
    snprintf(tb, sizeof(tb), "%s / %s", a, dur ? b : "--:--");
    lv_label_set_text(s_ui.time, loaded ? tb : "");

    set_icon(s_ui.play, st == PLAYER_PLAYING ? ICON_PAUSE : ICON_PLAY, g_pal.accent);
    set_icon(s_ui.shuffle, ICON_SHUFFLE, player_shuffle() ? g_pal.accent : g_pal.dim);
    player_repeat_t r = player_repeat();
    set_icon(s_ui.repeat, r == REPEAT_ONE ? ICON_REPEAT_ONE : r == REPEAT_ALL ? ICON_REPEAT : ICON_REPEAT_OFF,
             r == REPEAT_OFF ? g_pal.dim : g_pal.accent);
    deck_icon_text_set(s_ui.out, player_headphones() ? ICON_HEADPHONES : ICON_SPEAKER,
                       player_headphones() ? "HEADPHONES" : "SPEAKER", g_pal.dim);

    /* Highlight the playing row when this folder is the queue. */
    int idx      = strcmp(player_dir(), s_cwd) == 0 && loaded ? player_index() : -1;
    if (idx != s_ui.shown_index) {
        s_ui.shown_index = idx;
        for (int i = 0; i < s_entry_n; i++) {
            if (!s_ui.rows[i]) continue;
            bool on = !s_entries[i].dir && s_entries[i].track == idx;
            deck_panel_set_outline(s_ui.rows[i], on ? g_pal.accent : g_pal.line);
        }
    }
}

static void vol_changed(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
    player_set_volume(v);
    lv_label_set_text_fmt(s_ui.vol_val, "%d", v);
}

static void set_volume(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    player_set_volume(v);
    lv_slider_set_value(s_ui.vol, v, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ui.vol_val, "%d", v);
}

static void cycle_repeat(void) { player_set_repeat((player_repeat_t)((player_repeat() + 1) % 3)); }

static void ctl_clicked(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
        case 0: player_set_shuffle(!player_shuffle()); break;
        case 1: player_prev(); break;
        case 2: player_toggle(); break;
        case 3: player_next(); break;
        case 4: cycle_repeat(); break;
    }
    refresh(NULL);
}

static lv_obj_t *ctl_button(lv_obj_t *parent, const char *icon, intptr_t id, int32_t w)
{
    lv_obj_t *b = deck_button(parent, icon, g_font.icon_m);
    lv_obj_set_size(b, w, 72);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, ctl_clicked, LV_EVENT_CLICKED, (void *)id);
    return b;
}

/* Tap or drag on the progress bar to seek. */
static void progress_pressed(lv_event_t *e)
{
    lv_obj_t *bar = lv_event_get_target_obj(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(bar, &a);
    uint32_t dur = player_dur_ms();
    if (dur == 0 || lv_area_get_width(&a) <= 0) return;
    int32_t x       = p.x - a.x1;
    uint32_t target = (uint32_t)((uint64_t)dur * (uint32_t)(x < 0 ? 0 : x) / (uint32_t)lv_area_get_width(&a));
    player_seek(((int)target - (int)player_pos_ms()) / 1000);
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    if (s_entries == NULL) s_entries = calloc(MAX_ROWS, sizeof(entry_t));
    char r[192];
    music_root(r, sizeof(r));
    mkdir(r, 0755);
    if (s_cwd[0] == '\0' || strncmp(s_cwd, r, strlen(r)) != 0) snprintf(s_cwd, sizeof(s_cwd), "%s", r);

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_column(root, 20, 0);

    /* Left: library */
    lv_obj_t *left = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(left, 500, LV_PCT(100));
    deck_panel_set_tab(left, true);
    lv_obj_set_style_pad_all(left, 22, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);
    deck_section(left, "LIBRARY");
    s_ui.path = deck_label(left, g_font.mono_s, g_pal.dim, "");
    s_ui.list = deck_box(left);
    lv_obj_set_width(s_ui.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, 8, 0);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    s_ui.empty = deck_label(left, g_font.mono_m, g_pal.dim,
                            "No music yet.\n\nPut .mp3 files in /music on the SD card, or drop them in with FILES > PORTAL.");
    lv_obj_set_width(s_ui.empty, LV_PCT(100));
    lv_label_set_long_mode(s_ui.empty, LV_LABEL_LONG_MODE_WRAP);

    /* Right: now playing */
    lv_obj_t *right = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_all(right, 26, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 12, 0);
    deck_section(right, "NOW PLAYING");
    s_ui.title = deck_label(right, g_font.disp_m, g_pal.accent, "");
    lv_obj_set_width(s_ui.title, LV_PCT(100));
    lv_label_set_long_mode(s_ui.title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    s_ui.artist = deck_label(right, g_font.mono_m, g_pal.dim, "");
    lv_obj_set_width(s_ui.artist, LV_PCT(100));
    lv_label_set_long_mode(s_ui.artist, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *vis = deck_box(right);
    lv_obj_set_size(vis, LV_PCT(100), VIS_H);
    lv_obj_set_flex_flow(vis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < PLAYER_BANDS; i++) {
        lv_obj_t *b = lv_obj_create(vis);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 30, 4);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, g_pal.accent, 0);
        lv_obj_set_style_bg_grad_color(b, g_pal.accent2, 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        s_ui.bars[i] = b;
    }

    s_ui.progress = lv_bar_create(right);
    lv_obj_set_size(s_ui.progress, LV_PCT(100), 12);
    lv_bar_set_range(s_ui.progress, 0, 1000);
    lv_obj_set_style_bg_color(s_ui.progress, g_pal.line, 0);
    lv_obj_set_style_bg_opa(s_ui.progress, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ui.progress, g_pal.accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.progress, 0, 0);
    lv_obj_set_style_radius(s_ui.progress, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(s_ui.progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_ui.progress, 16);
    lv_obj_add_event_cb(s_ui.progress, progress_pressed, LV_EVENT_CLICKED, NULL);
    lv_obj_t *trow = deck_box(right);
    lv_obj_set_size(trow, LV_PCT(100), LV_SIZE_CONTENT);
    s_ui.time = deck_label(trow, g_font.mono_m, g_pal.text, "");
    lv_obj_align(s_ui.time, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.out = deck_icon_text(trow, ICON_SPEAKER, "SPEAKER", g_font.mono_s, g_pal.dim);
    lv_obj_align(s_ui.out, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *ctl = deck_box(right);
    lv_obj_set_size(ctl, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl, 12, 0);
    s_ui.shuffle = ctl_button(ctl, ICON_SHUFFLE, 0, 72);
    ctl_button(ctl, ICON_PREV, 1, 84);
    s_ui.play = ctl_button(ctl, ICON_PLAY, 2, 110);
    ctl_button(ctl, ICON_NEXT, 3, 84);
    s_ui.repeat = ctl_button(ctl, ICON_REPEAT, 4, 72);

    lv_obj_t *vrow = deck_box(right);
    lv_obj_set_size(vrow, LV_PCT(100), 44);
    lv_obj_t *vi = deck_label(vrow, g_font.icon_s, g_pal.dim, ICON_VOLUME);
    lv_obj_align(vi, LV_ALIGN_LEFT_MID, 0, 0);
    s_ui.vol = lv_slider_create(vrow);
    lv_obj_set_width(s_ui.vol, LV_PCT(80));
    lv_obj_align(s_ui.vol, LV_ALIGN_LEFT_MID, 44, 0);
    lv_slider_set_range(s_ui.vol, 0, 100);
    lv_slider_set_value(s_ui.vol, player_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_ui.vol, vol_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_ui.vol_val = deck_label(vrow, g_font.mono_m, g_pal.accent, "");
    lv_label_set_text_fmt(s_ui.vol_val, "%d", player_volume());
    lv_obj_align(s_ui.vol_val, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *hint = deck_label(right, g_font.mono_s, g_pal.dim,
                                "SPACE pause   LEFT/RIGHT seek   [ ] prev/next   - = volume\n"
                                "S shuffle   R repeat   ALT+P pause from anywhere");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);

    build_list();
    refresh(NULL);
    s_ui.timer = lv_timer_create(refresh, 40, NULL); /* ~25 fps for the bars */
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
        case HID_LEFT: player_seek(-10); return true;
        case HID_RIGHT: player_seek(10); return true;
        case HID_BACKSPACE:
            if (!at_root()) {
                char *slash = strrchr(s_cwd, '/');
                if (slash) *slash = '\0';
                build_list();
            }
            return true;
        default: break;
    }
    switch (k->key) {
        case ' ': player_toggle(); return true;
        case '[': player_prev(); return true;
        case ']': player_next(); return true;
        case '-': set_volume(player_volume() - 5); return true;
        case '=':
        case '+': set_volume(player_volume() + 5); return true;
        case 's':
        case 'S': player_set_shuffle(!player_shuffle()); return true;
        case 'r':
        case 'R': cycle_repeat(); return true;
        default: return false;
    }
}

static deck_app_t s_app = {
    .name     = "PLAYER",
    .tagline  = "AUDIO DECK",
    .icon     = ICON_MUSIC,
    .eta      = NULL,
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_player(void) { return &s_app; }
