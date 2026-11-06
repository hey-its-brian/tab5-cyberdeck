/*
 * Home screen and the APPS screen: a header, a tile area, and a scrolling
 * system ticker along the bottom.
 *
 * Home: the live panel on the left and the six fixed tiles in a 3x2 grid.
 * APPS: every optional app in a fixed slot (3x2 up to six, 4x3 up to twelve);
 * switched-off apps are greyed, and the first of them carries a quiet hint
 * pointing at SETTINGS > APPLICATIONS. Tiles show only an icon, a name and
 * the Alt+letter.
 */
#include "deck_launcher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "deck_boot.h"
#include "deck_hal.h"
#include "deck_hint.h"
#include "deck_live.h"
#include "deck_net.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define ROW_W 1232   /* content width inside the launcher padding */
#define TILE_H 470   /* height of the tile area */
#define TILE_GAP 18
#define PANEL_W 360  /* home: the live panel column */
#define MAX_TILES DECK_GROUP_APPS

static lv_obj_t *s_tiles[MAX_TILES];
static deck_tile_t s_items[MAX_TILES]; /* what each tile opens */
static bool s_off[MAX_TILES];          /* switched-off app: greyed, opens settings */
static int s_tile_count;
static int s_cols;                     /* grid columns, for up / down */
static deck_group_t *s_group;          /* NULL on the home screen */
static int s_sel_home;                 /* keyboard selection, survives trips into apps */
static int s_sel_group;
static deck_group_t *s_sel_group_of;   /* the group s_sel_group belongs to */
static bool s_kbd_nav;                 /* only show the selection once the keyboard is used */
static void (*s_manage)(const deck_app_t *focus);
static lv_obj_t *s_uptime;
static lv_timer_t *s_timer;
static lv_obj_t *s_stats;
static lv_timer_t *s_stats_timer;
static uint32_t s_stats_start;
static bool s_stats_settled;

void deck_launcher_set_manage(void (*fn)(const deck_app_t *focus)) { s_manage = fn; }

static int *sel(void) { return s_group ? &s_sel_group : &s_sel_home; }

static void open_item(int i)
{
    const deck_tile_t *it = &s_items[i];
    if (s_off[i]) {
        if (s_manage) s_manage(it->app); /* greyed: go and switch it on */
    } else if (it->group) {
        deck_shell_open_group(it->group);
    } else {
        deck_shell_launch(it->app);
    }
}

static void tile_clicked(lv_event_t *e)
{
    int i  = (int)(intptr_t)lv_event_get_user_data(e);
    *sel() = i;
    open_item(i);
}

static void show_selection(void)
{
    for (int i = 0; i < s_tile_count; i++) {
        if (s_kbd_nav && i == *sel()) {
            lv_obj_add_state(s_tiles[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_tiles[i], LV_STATE_CHECKED);
        }
    }
}

static void hint_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_shortcuts();
}

static char hotkey_letter(uint8_t hotkey) { return hotkey ? (char)('A' + hotkey - HID_A) : '\0'; }

/* Greyed text and icons: halfway between the dim text color and the
 * background, so a switched-off tile reads as present but asleep. */
static lv_color_t off_color(void) { return lv_color_mix(g_pal.dim, g_pal.bg, 128); }

/* Home tile styles (SETTINGS > STYLE > TILES, saved as "tile_style"): what
 * sets the home deck apart from the plainer APPS drawer.
 *   GLOW      an accent tint rising from each tile's top edge
 *   TWO-TONE  raised tiles with secondary-neon edges and an underline
 *   NEON      each home tile in its own fixed neon, washed like GLOW */
static const uint32_t s_neon[DECK_HOME_TILES] = {
    0x00F0FF, /* TERMINAL cyan */
    0xF3E600, /* NOTES yellow */
    0xFF2A6D, /* SETTINGS pink */
    0x39FF14, /* CALC green */
    0xB46CFF, /* FILES violet */
    0xFF8A1F, /* APPS orange */
};

int deck_tile_style(void)
{
    int s = (int)hal_cfg_get_i32("tile_style", STYLE_GLOW);
    return (s >= STYLE_GLOW && s <= STYLE_NEON) ? s : STYLE_GLOW;
}

/* Paints a home tile in the chosen style; returns the icon color. */
static lv_color_t style_home_tile(lv_obj_t *t, lv_obj_t *col, int index)
{
    switch (deck_tile_style()) {
        case STYLE_TWO_TONE: {
            deck_panel_set_fill(t, g_pal.panel_hi);
            deck_panel_set_outline(t, lv_color_mix(g_pal.accent2, g_pal.line, 110));
            lv_obj_t *u = deck_box(col); /* underline under the name */
            lv_obj_set_size(u, 56, 4);
            lv_obj_set_style_bg_color(u, g_pal.accent2, 0);
            lv_obj_set_style_bg_opa(u, LV_OPA_COVER, 0);
            return g_pal.accent;
        }
        case STYLE_NEON: {
            lv_color_t c = lv_color_hex(s_neon[index % DECK_HOME_TILES]);
            deck_panel_set_wash(t, true, c);
            deck_panel_set_outline(t, lv_color_mix(c, g_pal.line, 120));
            deck_panel_set_tab_color(t, c);
            return c;
        }
        default:
            deck_panel_set_wash(t, true, g_pal.accent);
            deck_panel_set_outline(t, lv_color_mix(g_pal.accent, g_pal.line, 110));
            return g_pal.accent;
    }
}

static lv_obj_t *build_tile(lv_obj_t *parent, int index, int32_t w, int32_t h)
{
    const deck_tile_t *it = &s_items[index];
    deck_app_t *app       = it->app;
    deck_group_t *grp     = it->group;
    bool offline          = app && app->eta != NULL;
    bool off              = s_off[index] && !offline; /* offline keeps its own look */

    lv_obj_t *t = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(t, w, h);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    deck_panel_set_tab(t, true);
    deck_panel_set_muted(t, off);
    deck_panel_set_hazard(t, offline);
    lv_obj_set_style_pad_all(t, 0, 0); /* the hint scrim covers the whole tile */
    lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    /* Top right: the Alt+letter; OFF when switched off; the version an
     * offline module is planned for. */
    char key[8] = {0};
    lv_color_t key_c = g_pal.accent2;
    if (offline) {
        snprintf(key, sizeof(key), "%s", app->eta);
        key_c = g_pal.warn;
    } else if (off) {
        snprintf(key, sizeof(key), "OFF");
        key_c = off_color();
    } else {
        key[0] = hotkey_letter(grp ? HID_A : app->hotkey);
    }
    lv_obj_t *k = deck_label(t, g_font.mono_s, key_c, key);
    lv_obj_align(k, LV_ALIGN_TOP_RIGHT, -18, 16);

    lv_color_t icon_c = off ? off_color() : offline ? g_pal.dim : g_pal.accent;
    lv_obj_t *col     = deck_box(t);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, h >= 200 ? 14 : 6, 0);
    lv_obj_center(col);
    lv_obj_t *icon = deck_label(col, h >= 200 ? g_font.icon_l : g_font.icon_m, icon_c, grp ? grp->icon : app->icon);
    deck_label(col, g_font.disp_m, off ? off_color() : g_pal.text, grp ? grp->name : app->name);
    if (s_group == NULL) {
        lv_color_t c = style_home_tile(t, col, index);
        lv_obj_set_style_text_color(icon, c, 0);
        if (deck_tile_style() == STYLE_NEON) lv_obj_set_style_text_color(k, c, 0);
    }
    return t;
}

/* The settings hint on the first switched-off tile: muted text over a light
 * scrim, readable when looked for, not loud on every visit. */
static void add_hint(lv_obj_t *tile)
{
    lv_obj_t *scrim = deck_box(tile);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_center(scrim);
    lv_obj_set_style_bg_color(scrim, g_pal.bg, 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_CLICKABLE); /* taps reach the tile */
    lv_obj_set_flex_flow(scrim, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scrim, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(scrim, 6, 0);
    deck_label(scrim, g_font.disp_s, g_pal.dim, "DISABLED");
    deck_label(scrim, g_font.mono_s, g_pal.dim, "ENABLE IN SETTINGS");
}

static void uptime_tick(lv_timer_t *t)
{
    (void)t;
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    uint32_t s = si.uptime_s;
    lv_label_set_text_fmt(s_uptime, "NODE %s  ::  UPTIME %02lu:%02lu:%02lu", si.board, (unsigned long)(s / 3600),
                          (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
}

static void launcher_deleted(lv_event_t *e)
{
    (void)e;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_stats_timer) {
        lv_timer_delete(s_stats_timer);
        s_stats_timer = NULL;
    }
    s_tile_count = 0;
    s_uptime     = NULL;
    s_stats      = NULL;
}

/* ---- Stats line ---------------------------------------------------------
 * On arrival the line "decodes": scrambled glyphs that lock in left to right
 * with a little jitter and color flicker, then it settles into a static
 * readout refreshed once a second. */

#define DECODE_MS 2400     /* total scramble time */
#define DECODE_TICK_MS 40
#define STATS_X 92

static const char *stats_text(void)
{
    static char buf[160];
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    hal_power_t p;
    hal_power_read(&p);

    char sd[24], pwr[24], net[32];
    if (net_state() == NET_CONNECTED) {
        snprintf(net, sizeof(net), "NET %s", net_ip());
    } else {
        snprintf(net, sizeof(net), "NET %s", net_state_name(net_state()));
    }
    if (hal_sd_mounted()) {
        snprintf(sd, sizeof(sd), "SD %.1fG FREE", (double)hal_sd_free_bytes() / 1e9);
    } else {
        snprintf(sd, sizeof(sd), "SD --");
    }
    if (!p.valid) {
        snprintf(pwr, sizeof(pwr), "PWR --");
    } else if (p.percent < 0) {
        snprintf(pwr, sizeof(pwr), "PWR USB");
    } else {
        snprintf(pwr, sizeof(pwr), "PWR %d%% %.2fV", p.percent, (double)p.volts);
    }
    snprintf(buf, sizeof(buf), "%s  //  PSRAM %.1f/%.0fM  //  SRAM %lu/%luK  //  %s  //  %s  //  KBD %s", net,
             (double)(si.psram_total - si.psram_free) / 1048576.0, (double)si.psram_total / 1048576.0,
             (unsigned long)((si.heap_int_total - si.heap_int_free) / 1024), (unsigned long)(si.heap_int_total / 1024),
             sd, pwr, hal_kbd_present() ? "LINK" : "DOWN");
    return buf;
}

static void stats_tick(lv_timer_t *t)
{
    const char *target = stats_text();
    if (s_stats_settled) {
        lv_label_set_text(s_stats, target);
        return;
    }

    /* The home screen is built behind the boot POST; hold the decode until
     * the POST is gone so it is actually seen. */
    if (deck_boot_active()) s_stats_start = lv_tick_get();

    uint32_t elapsed = lv_tick_elaps(s_stats_start);
    if (elapsed >= DECODE_MS) {
        s_stats_settled = true;
        lv_label_set_text(s_stats, target);
        lv_obj_set_style_text_color(s_stats, g_pal.text, 0);
        lv_obj_align(s_stats, LV_ALIGN_LEFT_MID, STATS_X, 0);
        lv_timer_set_period(t, 1000);
        return;
    }

    /* Characters lock in left to right over the last two thirds of the run;
     * before that everything is noise. */
    static const char noise[] = "!<>-_/\\[]{}=+*^?#%&0123456789ABCDEFXZ";
    size_t len      = strlen(target);
    int32_t lock_at = (int32_t)elapsed - DECODE_MS / 3;
    size_t locked   = lock_at <= 0 ? 0 : (size_t)((uint64_t)lock_at * len / (DECODE_MS * 2 / 3));
    char buf[160];
    for (size_t i = 0; i < len && i < sizeof(buf) - 1; i++) {
        char c = target[i];
        buf[i] = (i < locked || c == ' ') ? c : noise[lv_rand(0, sizeof(noise) - 2)];
    }
    buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
    lv_label_set_text(s_stats, buf);

    /* Occasional horizontal tear and color flicker. */
    bool tear = lv_rand(0, 9) < 3;
    lv_obj_align(s_stats, LV_ALIGN_LEFT_MID, STATS_X + (tear ? (int32_t)lv_rand(0, 12) - 6 : 0), 0);
    static const lv_color_t *flicker[3];
    flicker[0] = &g_pal.accent;
    flicker[1] = &g_pal.accent2;
    flicker[2] = &g_pal.text;
    lv_obj_set_style_text_color(s_stats, *flicker[tear ? lv_rand(0, 1) : 2], 0);
}

void deck_launcher_create(lv_obj_t *parent, deck_group_t *group)
{
    s_group = group;
    if (group != s_sel_group_of) {
        s_sel_group    = 0;
        s_sel_group_of = group;
    }

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_row(root, 20, 0);
    lv_obj_add_event_cb(root, launcher_deleted, LV_EVENT_DELETE, NULL);

    /* Header row */
    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_t *title = deck_label(head, g_font.disp_s, g_pal.accent, "// SELECT MODULE");
    if (group) lv_label_set_text_fmt(title, "// %s", group->name);
    s_uptime = deck_label(head, g_font.mono_s, g_pal.dim, "");
    lv_obj_align(s_uptime, LV_ALIGN_RIGHT_MID, 0, 0);
    /* Shortcut hint; tapping it opens the list too. */
    lv_obj_t *hint = deck_label(head, g_font.mono_s, g_pal.dim, group ? "ESC OR DEL: BACK TO DECK    SHORTCUTS: ALT+H" : "SHORTCUTS: ALT+H");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(hint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hint, 14);
    lv_obj_add_event_cb(hint, hint_clicked, LV_EVENT_CLICKED, NULL);

    /* Tiles: home is the live panel plus a 3x2 grid of the fixed tiles;
     * APPS is a full-width grid with a fixed slot per optional app. */
    s_tile_count = 0;
    if (group) {
        for (size_t i = 0; i < group->count && s_tile_count < MAX_TILES; i++) {
            deck_app_t *a           = group->apps[i];
            s_off[s_tile_count]     = !deck_app_enabled(a);
            s_items[s_tile_count++] = (deck_tile_t){.app = a};
        }
    } else {
        for (size_t i = 0; i < deck_home_count() && s_tile_count < MAX_TILES; i++) {
            s_off[s_tile_count]     = false;
            s_items[s_tile_count++] = *deck_home_tile(i);
        }
    }
    int n    = s_tile_count;
    s_cols   = (group && n > 6) ? 4 : 3;
    int rows = n > 6 ? 3 : 2; /* APPS: 3x2 up to 6, 4x3 for 7 to 12 */

    lv_obj_t *area = deck_box(root);
    lv_obj_set_size(area, ROW_W, TILE_H);
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(area, TILE_GAP, 0);
    int32_t grid_w = ROW_W;
    if (!group) {
        deck_live_create(area, PANEL_W, TILE_H);
        grid_w = ROW_W - PANEL_W - TILE_GAP;
    }
    lv_obj_t *grid = deck_box(area);
    lv_obj_set_size(grid, grid_w, TILE_H);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, TILE_GAP, 0);
    lv_obj_set_style_pad_row(grid, TILE_GAP, 0);
    int32_t tile_w = (grid_w - (s_cols - 1) * TILE_GAP) / s_cols;
    int32_t tile_h = (TILE_H - (rows - 1) * TILE_GAP) / rows;
    for (int i = 0; i < n; i++) {
        s_tiles[i] = build_tile(grid, i, tile_w, tile_h);
    }
    if (group) {
        bool offline[MAX_TILES];
        for (int i = 0; i < n; i++) offline[i] = s_items[i].app->eta != NULL;
        int h = deck_hint_slot(s_off, offline, (size_t)n);
        if (h >= 0) add_hint(s_tiles[h]);
    }
    if (*sel() >= s_tile_count) *sel() = 0;
    show_selection();

    /* Ticker */
    lv_obj_t *bar = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 12);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_style_pad_hor(bar, 14, 0);

    lv_obj_t *tag = deck_panel(bar, DECK_CUT_BR, 8);
    deck_panel_set_fill(tag, g_pal.accent);
    deck_panel_set_outline(tag, g_pal.accent);
    lv_obj_set_size(tag, 76, 30);
    lv_obj_align(tag, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *tl = deck_label(tag, g_font.disp_s, g_pal.bg, "SYS>");
    lv_obj_center(tl);

    s_stats = deck_label(bar, g_font.mono_m, g_pal.text, "");
    lv_label_set_long_mode(s_stats, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(s_stats, 1228 - 28 - STATS_X);
    lv_obj_align(s_stats, LV_ALIGN_LEFT_MID, STATS_X, 0);
    s_stats_start   = lv_tick_get();
    s_stats_settled = group != NULL; /* the decode plays on the home screen only */
    s_stats_timer   = lv_timer_create(stats_tick, group ? 1000 : DECODE_TICK_MS, NULL);
    stats_tick(s_stats_timer);

    uptime_tick(NULL);
    s_timer = lv_timer_create(uptime_tick, 1000, NULL);
}

bool deck_launcher_key(const deck_key_t *k)
{
    if (s_tile_count == 0) return false;

    int *s     = sel();
    int target = -1;
    switch (k->code) {
        case HID_LEFT:
            *s = s_kbd_nav ? (*s + s_tile_count - 1) % s_tile_count : *s;
            break;
        case HID_RIGHT:
        case HID_TAB:
            *s = s_kbd_nav ? (*s + 1) % s_tile_count : *s;
            break;
        case HID_UP:
            if (s_kbd_nav && *s - s_cols >= 0) *s -= s_cols;
            break;
        case HID_DOWN:
            if (s_kbd_nav && *s + s_cols < s_tile_count) *s += s_cols;
            break;
        case HID_ENTER:
        case HID_SPACE:
            if (s_kbd_nav) target = *s;
            break;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + s_tile_count) {
                target = k->code - HID_1;
                *s     = target;
            } else {
                return false;
            }
            break;
    }
    s_kbd_nav = true;
    show_selection();
    if (target >= 0) open_item(target);
    return true;
}
