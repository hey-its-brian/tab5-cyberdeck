/*
 * Home screen: one row of tall module tiles (big touch targets), a header,
 * and a scrolling system ticker along the bottom.
 */
#include "deck_launcher.h"

#include <stdio.h>
#include <string.h>

#include "deck_boot.h"
#include "deck_hal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TILE_W 226
#define TILE_H 470
#define TILE_GAP 22
#define MAX_TILES 9

static lv_obj_t *s_tiles[MAX_TILES];
static int s_tile_count;
static int s_sel;          /* keyboard selection, survives trips into apps */
static bool s_kbd_nav;     /* only show the selection once the keyboard is used */
static lv_obj_t *s_uptime;
static lv_timer_t *s_timer;
static lv_obj_t *s_stats;
static lv_timer_t *s_stats_timer;
static uint32_t s_stats_start;
static bool s_stats_settled;

static void tile_clicked(lv_event_t *e)
{
    deck_app_t *app = (deck_app_t *)lv_event_get_user_data(e);
    s_sel           = deck_app_index(app);
    deck_shell_launch(app);
}

static void show_selection(void)
{
    for (int i = 0; i < s_tile_count; i++) {
        if (s_kbd_nav && i == s_sel) {
            lv_obj_add_state(s_tiles[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_tiles[i], LV_STATE_CHECKED);
        }
    }
}

static lv_obj_t *build_tile(lv_obj_t *parent, deck_app_t *app, int index)
{
    lv_obj_t *t = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(t, TILE_W, TILE_H);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    deck_panel_set_tab(t, true);
    deck_panel_set_hazard(t, app->eta != NULL);
    lv_obj_set_style_pad_all(t, 18, 0);
    lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, app);

    char num[4];
    snprintf(num, sizeof(num), "%02d", index + 1);
    lv_obj_t *n = deck_label(t, g_font.disp_l, g_pal.accent2, num);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 4, 10);

    lv_obj_t *chip = app->eta ? deck_chip(t, app->eta, g_pal.warn) : deck_chip(t, "ONLINE", g_pal.ok);
    lv_obj_align(chip, LV_ALIGN_TOP_RIGHT, 0, 14);

    lv_obj_t *icon = deck_label(t, g_font.icon_l, app->eta ? g_pal.dim : g_pal.accent, app->icon);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *name = deck_label(t, g_font.disp_m, g_pal.text, app->name);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 52);

    lv_obj_t *tag = deck_label(t, g_font.mono_s, g_pal.dim, app->tagline);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 84);

    char hot[12];
    snprintf(hot, sizeof(hot), "ALT+%d", index + 1);
    lv_obj_t *h = deck_label(t, g_font.mono_s, g_pal.dim, hot);
    lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 4, -18);

    lv_obj_t *state = deck_label(t, g_font.mono_s, app->eta ? g_pal.warn : g_pal.ok,
                                 app->eta ? "OFFLINE" : "READY");
    lv_obj_align(state, LV_ALIGN_BOTTOM_RIGHT, -8, -18);
    return t;
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

    char sd[24], pwr[24];
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
    snprintf(buf, sizeof(buf), "CPU %luMHZ  //  PSRAM %.1f/%.0fM  //  SRAM %lu/%luK  //  %s  //  %s  //  KBD %s",
             (unsigned long)si.cpu_mhz, (double)(si.psram_total - si.psram_free) / 1048576.0,
             (double)si.psram_total / 1048576.0, (unsigned long)((si.heap_int_total - si.heap_int_free) / 1024),
             (unsigned long)(si.heap_int_total / 1024), sd, pwr, hal_kbd_present() ? "LINK" : "DOWN");
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

void deck_launcher_create(lv_obj_t *parent)
{
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
    deck_label(head, g_font.disp_s, g_pal.accent, "// SELECT MODULE");
    s_uptime = deck_label(head, g_font.mono_s, g_pal.dim, "");
    lv_obj_align(s_uptime, LV_ALIGN_RIGHT_MID, 0, 0);

    /* Tiles */
    lv_obj_t *row = deck_box(root);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, TILE_GAP, 0);
    s_tile_count = 0;
    for (size_t i = 0; i < deck_app_count() && i < MAX_TILES; i++) {
        s_tiles[s_tile_count++] = build_tile(row, deck_app_get(i), (int)i);
    }
    if (s_sel >= s_tile_count) s_sel = 0;
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
    s_stats_settled = false;
    s_stats_timer   = lv_timer_create(stats_tick, DECODE_TICK_MS, NULL);
    stats_tick(s_stats_timer);

    uptime_tick(NULL);
    s_timer = lv_timer_create(uptime_tick, 1000, NULL);
}

bool deck_launcher_key(const deck_key_t *k)
{
    if (s_tile_count == 0) return false;

    int target = -1;
    switch (k->code) {
        case HID_LEFT:
            s_sel = s_kbd_nav ? (s_sel + s_tile_count - 1) % s_tile_count : s_sel;
            break;
        case HID_RIGHT:
        case HID_TAB:
            s_sel = s_kbd_nav ? (s_sel + 1) % s_tile_count : s_sel;
            break;
        case HID_ENTER:
        case HID_SPACE:
            if (s_kbd_nav) target = s_sel;
            break;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + s_tile_count) {
                target = k->code - HID_1;
                s_sel  = target;
            } else {
                return false;
            }
            break;
    }
    s_kbd_nav = true;
    show_selection();
    if (target >= 0) {
        deck_shell_launch(deck_app_get((size_t)target));
    }
    return true;
}
