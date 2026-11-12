/*
 * RF WATCH: the RF Sentinel carrier on the Tab5's M5-Bus (a Stamp C5 with a
 * CC1101, an nRF24 and a GPS), as a suite of tools behind one door.
 *
 * Opening the app asks one question first: SAFE or RED TEAM. SAFE watches,
 * detects and logs; nothing in it can transmit. RED TEAM also holds the
 * node's arm lease, which is what transmit tests check before they run.
 * The choice holds until the app is left: to change it, leave and reopen.
 * Leaving by any route (Esc, Alt+Esc, another app's shortcut, switching
 * RF WATCH off, the screen sleeping) drops the lease.
 *
 * Behind the gate is a chooser of tools, drawn like the home screen. Going
 * back from a tool (Esc, Del or the status bar's button) returns to the
 * chooser; from the chooser or the gate it leaves RF WATCH.
 *
 * Whatever is on screen, while RF WATCH is switched on in SETTINGS the board
 * is powered and read, and a background timer here turns the board's
 * warnings into notices (the status bar count and a banner) that reach the
 * user in any app. That part is always in the safe posture.
 *
 * Screens: this file (gate, chooser), rf_overview.c, rf_wifi.c, rf_ble.c, rf_drive.c,
 * rf_log.c.
 */
#include "app_rf.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_notice.h"
#include "deck_saver.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "rf_ui.h"

static deck_app_t s_app;

/* ---- Tools --------------------------------------------------------------- */

typedef struct {
    const char *name, *tagline, *icon;
    const char *eta; /* NULL when it works; else the phase that brings it */
    void (*build)(lv_obj_t *parent);
    void (*destroy)(void);
    bool (*key)(const deck_key_t *k);
} rf_tool_t;

static const rf_tool_t s_tools[] = {
    {"OVERVIEW", "LINK, GNSS, SWEEPS, EVENTS", ICON_RADAR, NULL, rf_overview_build, rf_overview_destroy, rf_overview_key},
    {"WI-FI", "MONITOR, NETWORKS, DRONES", ICON_WIFI, NULL, rf_wifi_build, rf_wifi_destroy, rf_wifi_key},
    {"SUB-GHZ", "SPECTRUM, SURVEY, TPMS", ICON_ANTENNA, "PHASE 5", NULL, NULL, NULL},
    {"2.4 GHZ", "SPECTRUM, SURVEY, JAM DETECT", ICON_GAUGE, "PHASE 5", NULL, NULL, NULL},
    {"BLUETOOTH", "SCAN, TRACKERS, CAPTURE", ICON_WIRELESS, NULL, rf_ble_build, rf_ble_destroy, rf_ble_key},
    {"LOG", "EVENTS ON STORAGE", ICON_HISTORY, NULL, rf_log_build, rf_log_destroy, rf_log_key},
    {"WARDRIVE", "WI-FI AND BLUETOOTH WITH GPS", ICON_PIN, NULL, rf_drive_build, rf_drive_destroy, rf_drive_key},
    {"TESTS", "TARGETED, RED TEAM ONLY", ICON_BOLT, "PHASE 6", NULL, NULL, NULL},
};
#define TOOL_N    ((int)(sizeof(s_tools) / sizeof(s_tools[0])))
#define TOOL_COLS 4

typedef enum { SCR_NONE, SCR_GATE, SCR_CHOOSER, SCR_TOOL } rf_screen_t;

static struct {
    lv_obj_t   *parent;         /* the app's content area */
    rf_screen_t screen;
    rf_mode_t   mode;
    int         tool;           /* the tool on screen, or the chooser's selection */
    int         gate_sel;       /* 0 SAFE, 1 RED TEAM */
    lv_obj_t   *tile[TOOL_N];   /* gate uses the first two */
    lv_obj_t   *mode_chip, *link;
    lv_timer_t *timer;          /* chooser header refresh */
    sentinel_state_t st;
} s_ui DECK_PSRAM_BSS;

rf_mode_t rf_mode(void) { return s_ui.mode; }

lv_obj_t *rf_panel(lv_obj_t *parent, const char *title)
{
    lv_obj_t *p = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 16);
    lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT); /* the last panel in each column grows instead */
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(p, 14, 0);
    lv_obj_set_style_pad_row(p, 6, 0);
    deck_section(p, title);
    return p;
}

static void show_gate(void);
static void show_chooser(void);
static void show_tool(int i);

/* Tear down whatever screen is up. Timers first: they point at its widgets. */
static void clear_screen(void)
{
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    s_ui.timer = NULL;
    if (s_ui.screen == SCR_TOOL && s_tools[s_ui.tool].destroy) s_tools[s_ui.tool].destroy();
    if (s_ui.parent) lv_obj_clean(s_ui.parent);
    memset(s_ui.tile, 0, sizeof(s_ui.tile));
    s_ui.mode_chip = s_ui.link = NULL;
    s_ui.screen                = SCR_NONE;
}

static void select_tile(int count, int sel)
{
    for (int i = 0; i < count; i++) {
        if (!s_ui.tile[i]) continue;
        if (i == sel) lv_obj_add_state(s_ui.tile[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_ui.tile[i], LV_STATE_CHECKED);
    }
}

/* A launcher-style tile: icon, name, a line under it, a tag top right. */
static lv_obj_t *tile(lv_obj_t *parent, int32_t w, int32_t h, const char *icon, const char *name, const char *line,
                      const char *tag, lv_color_t color, bool offline)
{
    lv_obj_t *t = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(t, w, h);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    deck_panel_set_tab(t, true);
    deck_panel_set_tab_color(t, color);
    deck_panel_set_hazard(t, offline);
    if (!offline) {
        deck_panel_set_wash(t, true, color);
        deck_panel_set_outline(t, lv_color_mix(color, g_pal.line, 110));
    }
    if (tag) {
        lv_obj_t *k = deck_label(t, g_font.mono_s, offline ? g_pal.warn : color, tag);
        lv_obj_align(k, LV_ALIGN_TOP_RIGHT, -18, 16);
    }
    lv_obj_t *col = deck_box(t);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_center(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE); /* taps reach the tile */
    deck_label(col, h >= 300 ? g_font.icon_l : g_font.icon_m, offline ? g_pal.dim : color, icon);
    deck_label(col, g_font.disp_m, offline ? g_pal.dim : g_pal.text, name);
    lv_obj_t *l = deck_label(col, g_font.mono_s, g_pal.dim, line);
    lv_obj_set_width(l, LV_PCT(86));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return t;
}

static lv_obj_t *screen_root(const char *title)
{
    lv_obj_t *root = deck_box(s_ui.parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_row(root, 18, 0);
    lv_obj_t *head = deck_box(root);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 14, 0);
    deck_label(head, g_font.disp_s, g_pal.accent, title);
    return root;
}

/* ---- The mode gate ------------------------------------------------------- */

static void enter_mode(rf_mode_t mode)
{
    s_ui.mode = mode;
    if (mode == RF_MODE_RED) {
        sentinel_arm(true);
        sentinel_note(false, "red team session started");
        deck_notice_set_flag("RED TEAM");
    }
    show_chooser();
}

static void gate_clicked(lv_event_t *e) { enter_mode((rf_mode_t)(intptr_t)lv_event_get_user_data(e)); }

static void show_gate(void)
{
    clear_screen();
    s_ui.screen   = SCR_GATE;
    s_ui.gate_sel = 0; /* Enter straight through always lands on SAFE */
    lv_obj_t *root = screen_root("// RF WATCH  ::  CHOOSE THIS SESSION'S MODE");

    lv_obj_t *row = deck_box(root);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 28, 0);
    s_ui.tile[0] = tile(row, 560, 420, ICON_RADAR, "SAFE", "WATCH, DETECT AND LOG. NOTHING TRANSMITS.", "1", g_pal.ok, false);
    s_ui.tile[1] = tile(row, 560, 420, ICON_BOLT, "RED TEAM",
                        "TRANSMIT TESTS BECOME AVAILABLE. USE THEM ONLY ON EQUIPMENT YOU OWN, OR WITH THE OWNER'S "
                        "FULL PERMISSION AND KNOWLEDGE.",
                        "2", g_pal.danger, false);
    lv_obj_add_event_cb(s_ui.tile[0], gate_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)RF_MODE_SAFE);
    lv_obj_add_event_cb(s_ui.tile[1], gate_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)RF_MODE_RED);
    select_tile(2, s_ui.gate_sel);

    deck_label(root, g_font.mono_s, g_pal.dim,
               "[LEFT/RIGHT] CHOOSE   [ENTER] START   [ESC] BACK      THE MODE HOLDS UNTIL YOU LEAVE RF WATCH. "
               "TO CHANGE IT, LEAVE AND REOPEN.");
}

static bool gate_key(const deck_key_t *k)
{
    switch (k->code) {
        case HID_LEFT:
        case HID_RIGHT:
        case HID_TAB:
            s_ui.gate_sel ^= 1;
            select_tile(2, s_ui.gate_sel);
            return true;
        case HID_ENTER:
        case HID_SPACE:
            enter_mode(s_ui.gate_sel ? RF_MODE_RED : RF_MODE_SAFE);
            return true;
        case HID_1:
            enter_mode(RF_MODE_SAFE);
            return true;
        case HID_1 + 1:
            enter_mode(RF_MODE_RED);
            return true;
        default: return false;
    }
}

/* ---- The chooser --------------------------------------------------------- */

static void chooser_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_ui.mode_chip) return;
    sentinel_snapshot(&s_ui.st);
    const sentinel_state_t *st = &s_ui.st;
    bool red = s_ui.mode == RF_MODE_RED, link = sentinel_link(st);
    lv_obj_t *chip_label = lv_obj_get_child(s_ui.mode_chip, 0);
    lv_color_t c         = red ? g_pal.danger : g_pal.ok;
    lv_label_set_text(chip_label, red ? "RED TEAM" : "SAFE");
    lv_obj_set_style_text_color(chip_label, c, 0);
    deck_panel_set_outline(s_ui.mode_chip, c);
    const char *arm = !red ? "TRANSMIT TESTS LOCKED" : sentinel_armed(st) ? "NODE ARMED" : "NODE NOT ARMED YET";
    if (!st->powered) lv_label_set_text(s_ui.link, "BOARD OFF");
    else if (!link) lv_label_set_text_fmt(s_ui.link, "%s  ::  WAITING FOR THE C5", arm);
    else lv_label_set_text_fmt(s_ui.link, "%s  ::  LINK %u BAUD  ::  FW %s", arm, (unsigned)st->host_baud, st->fw);
}

static void open_tool(int i)
{
    if (i < 0 || i >= TOOL_N) return;
    s_ui.tool = i;
    select_tile(TOOL_N, i);
    if (s_tools[i].build) show_tool(i); /* an offline tile only takes the selection */
}

static void tool_clicked(lv_event_t *e) { open_tool((int)(intptr_t)lv_event_get_user_data(e)); }

static void show_chooser(void)
{
    int keep = s_ui.tool; /* coming back from a tool: the selection stays on it */
    clear_screen();
    s_ui.screen = SCR_CHOOSER;
    s_ui.tool   = keep;
    lv_obj_t *root = screen_root("// RF WATCH  ::  TOOLS");
    lv_obj_t *head = lv_obj_get_child(root, 0);
    s_ui.mode_chip = deck_chip(head, "SAFE", g_pal.ok);
    s_ui.link      = deck_label(head, g_font.mono_s, g_pal.dim, "");

    lv_obj_t *grid = deck_box(root);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, 20, 0);
    lv_obj_set_style_pad_row(grid, 20, 0);
    for (int i = 0; i < TOOL_N; i++) {
        const rf_tool_t *tl = &s_tools[i];
        char             num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        s_ui.tile[i] = tile(grid, 290, 250, tl->icon, tl->name, tl->tagline, tl->eta ? tl->eta : num, g_pal.accent,
                            tl->eta != NULL);
        lv_obj_add_event_cb(s_ui.tile[i], tool_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    select_tile(TOOL_N, s_ui.tool);

    deck_label(root, g_font.mono_s, g_pal.dim,
               "[ARROWS] CHOOSE   [ENTER] OPEN   [1-8] JUMP   [ESC] LEAVE RF WATCH (ENDS THE SESSION)");
    s_ui.timer = lv_timer_create(chooser_tick, 500, NULL);
    chooser_tick(s_ui.timer);
}

static bool chooser_key(const deck_key_t *k)
{
    int s = s_ui.tool;
    switch (k->code) {
        case HID_LEFT: s = (s + TOOL_N - 1) % TOOL_N; break;
        case HID_RIGHT:
        case HID_TAB: s = (s + 1) % TOOL_N; break;
        case HID_UP:
            if (s - TOOL_COLS >= 0) s -= TOOL_COLS;
            break;
        case HID_DOWN:
            if (s + TOOL_COLS < TOOL_N) s += TOOL_COLS;
            break;
        case HID_ENTER:
        case HID_SPACE:
            open_tool(s);
            return true;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + TOOL_N) {
                open_tool(k->code - HID_1);
                return true;
            }
            return false;
    }
    s_ui.tool = s;
    select_tile(TOOL_N, s);
    return true;
}

static void show_tool(int i)
{
    clear_screen();
    s_ui.screen = SCR_TOOL;
    s_ui.tool   = i;
    s_tools[i].build(s_ui.parent);
}

/* ---- App callbacks ------------------------------------------------------- */

/* The session ends: back to the safe posture, whatever it was. */
static void end_session(const char *why)
{
    if (s_ui.mode == RF_MODE_RED) {
        sentinel_arm(false);
        sentinel_note(false, why);
    }
    deck_notice_set_flag(NULL);
    s_ui.mode = RF_MODE_SAFE;
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.parent = parent;
    deck_notice_seen();
    show_gate();
    return true;
}

/* on_exit runs before the shell deletes the widgets, by whichever route the
 * app is left. The lease goes first. */
static void leave(deck_app_t *self)
{
    (void)self;
    end_session("red team session ended");
}

static void stop(deck_app_t *self)
{
    (void)self;
    /* The shell has already cleaned the content area: only our own state is left. */
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    if (s_ui.screen == SCR_TOOL && s_tools[s_ui.tool].destroy) s_tools[s_ui.tool].destroy();
    memset(&s_ui, 0, sizeof(s_ui));
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    switch (s_ui.screen) {
        case SCR_GATE: return gate_key(k);
        case SCR_CHOOSER: return chooser_key(k);
        case SCR_TOOL:
            if (k->code == HID_ESC) return false; /* the shell's back: on_back takes it to the chooser */
            return s_tools[s_ui.tool].key ? s_tools[s_ui.tool].key(k) : false;
        default: return false;
    }
}

/* Back from a tool is the chooser, by whichever route: Esc, Del or the
 * status bar's button. From the chooser and the gate the shell leaves. */
static bool on_back(deck_app_t *self)
{
    (void)self;
    if (s_ui.screen != SCR_TOOL) return false;
    show_chooser();
    return true;
}

/* ---- Background: warnings reach the user anywhere ------------------------- */

static struct {
    lv_timer_t *timer;
    uint32_t    seen_seq;
    sentinel_state_t st;
} s_bg DECK_PSRAM_BSS;

static void bg_tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_bg.st);
    const sentinel_state_t *st = &s_bg.st;

    /* Every warning since the last look becomes a notice, oldest first. The
     * ring holds eight; a longer burst loses its oldest here (the log has
     * them all). A sequence that went backwards means the board restarted. */
    if (st->evt_seq < s_bg.seen_seq) s_bg.seen_seq = 0;
    uint32_t fresh = st->evt_seq - s_bg.seen_seq;
    s_bg.seen_seq  = st->evt_seq;
    if (fresh > st->evt_count) fresh = st->evt_count;
    for (int i = (int)fresh - 1; i >= 0; i--) {
        const sentinel_event_t *e = sentinel_event(st, i);
        if (e && e->warn) deck_notice_post(DECK_NOTICE_WARN, e->src, e->text, &s_app);
    }
    bool open = deck_shell_current() == &s_app;
    if (open) deck_notice_seen();

    /* A sleeping screen is an unattended deck: RED TEAM does not survive it. */
    if (open && s_ui.mode == RF_MODE_RED && (deck_shell_sleeping() || deck_saver_active()))
        end_session("disarmed: screen slept");
}

/* ---- Home panel ---------------------------------------------------------- */

static struct {
    lv_obj_t   *head, *line;
    lv_timer_t *timer;
    sentinel_state_t st;
} s_tile DECK_PSRAM_BSS;

static void tile_tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_tile.st);
    const sentinel_state_t *st = &s_tile.st;
    bool link = sentinel_link(st);
    deck_icon_text_set(s_tile.head, ICON_ANTENNA, link ? "SENTINEL ONLINE" : st->powered ? "SENTINEL: NO LINK" : "SENTINEL OFF",
                       link ? g_pal.ok : g_pal.warn);
    if (!link) {
        lv_label_set_text(s_tile.line, st->powered ? "Board powered, waiting for the C5." : "Switch RF WATCH on in SETTINGS.");
        return;
    }
    int best = -127, busy = 0;
    for (int i = 0; i < SENTINEL_SUB_CH; i++)
        if (st->sub[i] > best) best = st->sub[i];
    for (int i = 0; i < SENTINEL_NRF_CH; i++)
        if (st->nrf[i] >= 4) busy++;
    lv_label_set_text_fmt(s_tile.line, "GPS %d SATS %s    433 MHZ %d dBm    2.4 GHZ %d BUSY    WI-FI %u    BLE %u", st->sats_used,
                          st->fix ? "FIX" : "NO FIX", best, busy, st->wifi_total, st->ble_devices);
}

static void tile_deleted(lv_event_t *e)
{
    (void)e;
    if (s_tile.timer) lv_timer_delete(s_tile.timer);
    memset(&s_tile, 0, sizeof(s_tile));
}

static void on_panel(deck_app_t *self, lv_obj_t *body)
{
    (void)self;
    memset(&s_tile, 0, sizeof(s_tile));
    lv_obj_add_event_cb(body, tile_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 4, 0);
    s_tile.head = deck_icon_text(body, ICON_ANTENNA, "", g_font.mono_m, g_pal.dim);
    s_tile.line = deck_label(body, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_width(s_tile.line, LV_PCT(100));
    lv_label_set_long_mode(s_tile.line, LV_LABEL_LONG_MODE_DOTS);
    s_tile.timer = lv_timer_create(tile_tick, 1000, NULL);
    tile_tick(s_tile.timer);
}

/* ---- Module -------------------------------------------------------------- */

/* Switching RF WATCH on powers the board (EXT5V), puts the CC1101 in receive
 * and starts reading the C5; switching it off cuts the power again. */
static void enable(deck_app_t *self)
{
    (void)self;
    sentinel_start();
    if (!s_bg.timer) {
        s_bg.seen_seq = 0;
        s_bg.timer    = lv_timer_create(bg_tick, 500, NULL);
    }
}

static void disable(deck_app_t *self)
{
    (void)self;
    end_session("red team session ended");
    if (s_bg.timer) lv_timer_delete(s_bg.timer);
    s_bg.timer = NULL;
    deck_notice_seen();
    sentinel_stop();
}

static deck_app_t s_app = {
    .name     = "RF WATCH",
    .tagline  = "SENTINEL BOARD",
    .icon     = ICON_ANTENNA,
    .eta      = NULL,
    .hotkey   = HID_LETTER('X'),
    .id       = "rf",
    .desc     = "RF Sentinel on the M5-Bus: watch, detect, log, test",
    .optional   = true,
    .default_on = false,
    .on_enable  = enable,
    .on_disable = disable,
    .on_start = start,
    .on_exit  = leave,
    .on_stop  = stop,
    .on_key   = on_key,
    .on_back  = on_back,
    .on_panel = on_panel,
};

deck_app_t *app_rf(void) { return &s_app; }
