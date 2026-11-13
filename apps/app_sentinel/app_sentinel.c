/*
 * SENTINEL: everything the deck knows about the air and the network round
 * it, as one module. It took over from two (RF WATCH, the radio board's
 * tools, and NET TOOLS, the deck's own Wi-Fi) and makes no distinction
 * between them: screens are grouped by what they are for, not by which
 * radio answers.
 *
 * It opens on a page of tiles, always in DEFENSIVE. Each tile leads to a
 * screen that does one job, or to another page of tiles. The frame round
 * every screen is in sn_frame.c; the screens are in one file per area.
 *
 * The radio board (a Stamp C5 with a CC1101, an nRF24 and a GPS on the
 * M5-Bus) is switched on and off on BOARD > RADIO BOARD. While it is on it
 * is powered and read whatever is on screen, and a background timer here
 * turns its warnings into notices (the status bar count and a banner) that
 * reach the user in any app.
 *
 * OFFENSIVE is the area of tools that transmit. Entering it asks first and
 * takes the node's arm lease; leaving it by any route (BACK, another app,
 * the screen sleeping, the board switched off) drops the lease.
 */
#include "app_sentinel.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_notice.h"
#include "deck_saver.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "sn_ui.h"

static deck_app_t s_app;

/* ---- The home page --------------------------------------------------------- */

static const sn_screen_t *const s_home_kids[] = {
    &sn_wifi_menu, &sn_ble_menu, &sn_spectrum_menu, &sn_net_menu, &sn_drive_screen, &sn_log_screen, &sn_board_menu, &sn_offense_menu,
};

static const sn_screen_t s_home = {
    .name    = "SENTINEL",
    .purpose = "What is on the air and the network around you. Choose an area.",
    .kids    = s_home_kids,
    .nkids   = (int)(sizeof(s_home_kids) / sizeof(s_home_kids[0])),
};

/* ---- The radio board's power ------------------------------------------------ */

static struct {
    lv_timer_t *timer;
    uint32_t    seen_seq;
    sentinel_state_t st;
} s_bg DECK_PSRAM_BSS;

static void bg_tick(lv_timer_t *t);

/* A deck that had RF WATCH switched on keeps its board on. */
bool sn_board_enabled(void) { return hal_cfg_get_i32("sn_board", hal_cfg_get_i32("app_rf", 0)) != 0; }

static void board_apply(bool on)
{
    if (on) {
        sentinel_start();
        if (!s_bg.timer) {
            s_bg.seen_seq = 0;
            s_bg.timer    = lv_timer_create(bg_tick, 500, NULL);
        }
    } else {
        sn_frame_exit("offensive mode ended: board switched off");
        if (s_bg.timer) lv_timer_delete(s_bg.timer);
        s_bg.timer = NULL;
        deck_notice_seen();
        sentinel_stop();
    }
}

void sn_board_set_enabled(bool on)
{
    hal_cfg_set_i32("sn_board", on ? 1 : 0);
    board_apply(on);
}

/* ---- Background: warnings reach the user anywhere ---------------------------- */

static void bg_tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_bg.st);
    const sentinel_state_t *st = &s_bg.st;

    /* Every warning since the last look becomes a notice, oldest first. The
     * ring holds eight; a longer burst loses its oldest here (the log has
     * them all). A sequence that went backwards means the board restarted.
     * Notes (a 433 MHz channel coming up, a fix gained) are for the log only. */
    if (st->evt_seq < s_bg.seen_seq) s_bg.seen_seq = 0;
    uint32_t fresh = st->evt_seq - s_bg.seen_seq;
    s_bg.seen_seq  = st->evt_seq;
    if (fresh > st->evt_count) fresh = st->evt_count;
    for (int i = (int)fresh - 1; i >= 0; i--) {
        const sentinel_event_t *e = sentinel_event(st, i);
        if (e && sn_level_of(e->warn, e->text) != SN_NOTE) deck_notice_post(DECK_NOTICE_WARN, e->src, e->text, &s_app);
    }

    /* A sleeping screen is an unattended deck: OFFENSIVE does not survive it. */
    if (sn_mode() == SN_OFFENSIVE && (deck_shell_sleeping() || deck_saver_active())) sn_frame_offense_slept();
}

/* ---- App callbacks ----------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    /* Opened from a warning (the banner or the status bar's count): go to what was warned about. */
    bool from_warning = deck_notice_unseen() > 0;
    sn_frame_start(parent, &s_home, &sn_log_screen);
    if (from_warning) sn_open(&sn_log_screen);
    return true;
}

/* on_exit runs before the shell deletes the widgets, by whichever route the
 * app is left. The lease goes first. */
static void leave(deck_app_t *self)
{
    (void)self;
    sn_frame_exit("offensive mode ended: left SENTINEL");
}

static void stop(deck_app_t *self)
{
    (void)self;
    sn_frame_stop();
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    return sn_frame_key(k);
}

static bool on_back(deck_app_t *self)
{
    (void)self;
    return sn_frame_back();
}

/* ---- The line on the DECK//OS home screen ------------------------------------- */

static struct {
    lv_obj_t   *head, *line;
    lv_timer_t *timer;
    sentinel_state_t st;
} s_tile DECK_PSRAM_BSS;

static void tile_tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_tile.st);
    const sentinel_state_t *st      = &s_tile.st;
    const char             *problem = sn_board_problem(st);
    unsigned                unseen  = deck_notice_unseen();
    if (problem) {
        deck_icon_text_set(s_tile.head, ICON_ANTENNA, problem, g_pal.warn);
        lv_label_set_text(s_tile.line, sn_board_enabled() ? "Powered, waiting for it to answer." : "Switch it on in SENTINEL > BOARD.");
        return;
    }
    /* What matters first: is anything wrong. */
    const sentinel_event_t *e = st->evt_count ? sentinel_event(st, 0) : NULL;
    if (st->nrf_band == SENTINEL_BAND_UP || st->sub_band == SENTINEL_BAND_UP) {
        deck_icon_text_set(s_tile.head, ICON_ALERT, st->nrf_band == SENTINEL_BAND_UP ? "!! 2.4 GHZ SATURATED" : "!! 433 MHZ BAND UP", g_pal.danger);
    } else if (unseen) {
        char head[40];
        snprintf(head, sizeof(head), "! %u WARNING%s NOT SEEN", unseen, unseen == 1 ? "" : "S");
        deck_icon_text_set(s_tile.head, ICON_ALERT, head, g_pal.warn);
    } else {
        deck_icon_text_set(s_tile.head, ICON_ANTENNA, "SENTINEL: ALL CLEAR", g_pal.ok);
    }
    if (unseen && e) lv_label_set_text_fmt(s_tile.line, "Latest: %s", e->text);
    else
        lv_label_set_text_fmt(s_tile.line, "Wi-Fi %u    Bluetooth %u    GPS %s", st->wifi_total, st->ble_devices, st->fix ? "fix" : "no fix");
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

/* ---- Module -------------------------------------------------------------------- */

/* Once at boot: the board comes up if its setting says so. */
static void enable(deck_app_t *self)
{
    (void)self;
    if (sn_board_enabled()) board_apply(true);
}

static deck_app_t s_app = {
    .name     = "SENTINEL",
    .tagline  = "AIR + NETWORK WATCH",
    .icon     = ICON_RADAR,
    .eta      = NULL,
    .hotkey   = HID_LETTER('X'),
    .id       = "sentinel",
    .desc     = "Wi-Fi, Bluetooth, radio bands and network tools",
    .on_enable = enable,
    .on_start = start,
    .on_exit  = leave,
    .on_stop  = stop,
    .on_key   = on_key,
    .on_back  = on_back,
    .on_panel = on_panel,
};

deck_app_t *app_sentinel(void) { return &s_app; }
