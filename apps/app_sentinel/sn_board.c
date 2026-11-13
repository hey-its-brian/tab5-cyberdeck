/*
 * SENTINEL > BOARD: the radio board itself (a Stamp C5 with a 433 MHz
 * radio, a 2.4 GHz radio and a GPS receiver on the M5-Bus).
 *
 *   RADIO BOARD  is it on, linked and whole; switch it on or off
 *   GPS          is there a position fix, and where
 *   DIAGNOSTICS  the link's counters and two test commands, for when it
 *                misbehaves
 */
#include "sn_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_theme.h"

#define TICK_MS 500

static struct {
    lv_obj_t   *state, *v[8];
    lv_timer_t *timer;
    void (*render)(void);
    sentinel_state_t st;
} s_bd DECK_PSRAM_BSS;

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_bd.st);
    if (s_bd.render) s_bd.render();
}

static void begin(void (*render)(void))
{
    memset(&s_bd, 0, sizeof(s_bd));
    s_bd.render = render;
    sentinel_snapshot(&s_bd.st);
    s_bd.timer = lv_timer_create(tick, TICK_MS, NULL);
}

static void end(void)
{
    if (s_bd.timer) lv_timer_delete(s_bd.timer);
    s_bd.timer  = NULL;
    s_bd.render = NULL;
}

static void fmt_uptime(char *out, size_t n, uint32_t s)
{
    if (s < 3600) snprintf(out, n, "%u MIN", (unsigned)(s / 60));
    else snprintf(out, n, "%u H %u MIN", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
}

/* ---- RADIO BOARD --------------------------------------------------------------- */

static void power_toggle(void);

static void part(lv_obj_t *l, const char *name, bool link, bool ok)
{
    if (!link) sn_set(l, g_pal.label, "%-22s --", name);
    else if (ok) sn_set(l, g_pal.ok, "%-22s OK", name);
    else sn_set(l, g_pal.danger, "%-22s !! NOT ANSWERING", name);
}

static void board_render(void)
{
    const sentinel_state_t *st   = &s_bd.st;
    bool                    on   = sn_board_enabled(), link = on && sentinel_link(st);
    char                    up[24];
    if (!on) {
        sn_set(s_bd.state, g_pal.warn, "SWITCHED OFF");
        sn_set(s_bd.v[0], g_pal.text, "Nothing on the air is being watched: no Wi-Fi, Bluetooth or band warnings, and no GPS.");
    } else if (!st->powered) {
        sn_set(s_bd.state, g_pal.danger, "!! COULD NOT BE POWERED");
        sn_set(s_bd.v[0], g_pal.text, "The deck could not switch the board's power on. See the log.");
    } else if (!link) {
        sn_set(s_bd.state, g_pal.warn, st->frames ? "! LINK LOST" : "! ON, NOT ANSWERING");
        sn_set(s_bd.v[0], g_pal.text, "The board is powered and the deck is waiting to hear from it. Is it seated on the M5-Bus?");
    } else {
        fmt_uptime(up, sizeof(up), st->uptime_s);
        sn_set(s_bd.state, g_pal.ok, "ON AND LINKED");
        sn_set(s_bd.v[0], g_pal.text, "Firmware %s, running for %s.", st->fw, up);
    }
    part(s_bd.v[1], "433 MHZ RADIO", link, st->cc_ok);
    part(s_bd.v[2], "2.4 GHZ RADIO", link, st->nrf_ok);
    part(s_bd.v[3], "GPS RECEIVER", link, st->gps_ok);
    part(s_bd.v[4], "WI-FI AND BLUETOOTH", link, st->wifi_ok);
    sn_action_label(power_toggle, on ? "SWITCH THE BOARD OFF" : "SWITCH THE BOARD ON");
}

static void power_off_yes(void)
{
    sn_board_set_enabled(false);
    sentinel_snapshot(&s_bd.st);
    board_render();
}

static void power_toggle(void)
{
    if (sn_board_enabled()) {
        sn_confirm("SWITCH THE RADIO BOARD OFF?",
                   "Nothing on the air will be watched until it is switched on again: no warnings about trackers, Wi-Fi "
                   "attacks or jamming, and no GPS.\n\nThe NETWORK tools keep working: they use the deck's own Wi-Fi.",
                   "SWITCH IT OFF", power_off_yes);
        return;
    }
    sn_board_set_enabled(true);
    sentinel_snapshot(&s_bd.st);
    board_render();
}

static void board_build(lv_obj_t *body)
{
    begin(board_render);
    s_bd.state = sn_state(body);
    s_bd.v[0]  = sn_value(body);
    sn_heading(body, "WHAT IS ON IT");
    for (int i = 1; i < 5; i++) s_bd.v[i] = sn_value(body);
    sn_action(HID_LETTER('P'), "P", "SWITCH THE BOARD OFF", power_toggle);
    sn_help("The radio board does all the listening: Wi-Fi and Bluetooth with its own radio, the 433 MHz and 2.4 GHz "
            "bands with two more, and position from its GPS receiver.\n\n"
            "While it is on it watches all the time, whatever is on the deck's screen, and its warnings appear in the "
            "status bar. Switching it off saves power.");
    board_render();
}

/* ---- GPS ----------------------------------------------------------------------- */

static void gps_render(void)
{
    const sentinel_state_t *st      = &s_bd.st;
    const char             *problem = sn_board_problem(st);
    bool                    have    = !problem && st->gps_ok;
    if (problem) sn_set(s_bd.state, g_pal.warn, "%s", problem);
    else if (!st->gps_ok) sn_set(s_bd.state, g_pal.danger, "!! THE GPS IS SILENT");
    else if (st->fix) sn_set(s_bd.state, g_pal.ok, "POSITION FIX");
    else sn_set(s_bd.state, g_pal.warn, "NO FIX YET");
    if (have && st->fix) {
        sn_set(s_bd.v[0], lv_color_white(), "%.6f,  %.6f", st->lat, st->lon);
        sn_set(s_bd.v[1], g_pal.text, "HEIGHT %.0f M", (double)st->alt_m);
    } else {
        sn_set(s_bd.v[0], g_pal.label, "---.------,  ---.------");
        sn_set(s_bd.v[1], g_pal.label, "%s", have ? "A fix needs a view of the sky, and a few minutes from cold." : " ");
    }
    sn_set(s_bd.v[2], g_pal.text, "SATELLITES: %d USED, %d IN VIEW", have ? st->sats_used : 0, have ? st->sats_view : 0);
    sn_set(s_bd.v[3], g_pal.text, "SIGNAL: %.0f AVERAGE, %d BEST (dB-Hz)", have ? (double)st->cn0_avg : 0.0, have ? st->cn0_max : 0);
    if (strlen(st->utc) >= 6) sn_set(s_bd.v[4], g_pal.text, "TIME FROM THE SATELLITES: %.2s:%.2s:%.2s UTC", st->utc, st->utc + 2, st->utc + 4);
    else sn_set(s_bd.v[4], g_pal.label, "TIME FROM THE SATELLITES: NOT YET");
}

static void gps_build(lv_obj_t *body)
{
    begin(gps_render);
    s_bd.state = sn_state(body);
    s_bd.v[0]  = sn_value(body);
    lv_obj_set_style_text_font(s_bd.v[0], g_font.disp_l, 0);
    s_bd.v[1] = sn_value(body);
    sn_heading(body, "HOW GOOD");
    for (int i = 2; i < 5; i++) s_bd.v[i] = sn_value(body);
    gps_render();
}

/* ---- DIAGNOSTICS ----------------------------------------------------------------- */

static void diag_render(void)
{
    const sentinel_state_t *st   = &s_bd.st;
    bool                    link = sentinel_link(st);
    if (link) sn_set(s_bd.state, g_pal.ok, "LINKED AT %u BAUD", (unsigned)st->host_baud);
    else sn_set(s_bd.state, g_pal.warn, "NOT LINKED");
    sn_set(s_bd.v[0], st->bad ? g_pal.warn : g_pal.text, "MESSAGES: %u RECEIVED, %u DAMAGED", (unsigned)st->frames, (unsigned)st->bad);
    sn_set(s_bd.v[1], g_pal.text, "FIRMWARE %s, PROTOCOL %s, FREE MEMORY %u K", st->fw[0] ? st->fw : "?", st->proto[0] ? st->proto : "?", (unsigned)(st->heap / 1024));
    char tool[sizeof(st->mode)];
    snprintf(tool, sizeof(tool), "%s", st->mode);
    sn_upper(tool);
    sn_set(s_bd.v[2], g_pal.text, "RADIO TOOL: %s", tool[0] ? tool : "?");
    if (sn_mode() == SN_OFFENSIVE)
        sn_set(s_bd.v[3], sentinel_armed(st) ? g_pal.danger : g_pal.warn, "OFFENSIVE: %s", sentinel_armed(st) ? "THE BOARD IS ARMED" : "THE BOARD IS NOT ARMED YET");
    else sn_set(s_bd.v[3], g_pal.text, "DEFENSIVE: THE BOARD WILL NOT TRANSMIT");
    for (int i = 0; i < 3; i++) {
        const sentinel_event_t *e = i < st->evt_count ? sentinel_event(st, i) : NULL;
        if (e) sn_set(s_bd.v[4 + i], g_pal.label, "%-5s %s", e->src, e->text);
        else sn_set(s_bd.v[4 + i], g_pal.label, "%s", i == 0 ? "No messages yet." : " ");
    }
}

static void diag_hello(void) { sentinel_send("CMD,HELLO"); }
static void diag_ping(void) { sentinel_send("CMD,PING"); }

static void diag_build(lv_obj_t *body)
{
    begin(diag_render);
    s_bd.state = sn_state(body);
    for (int i = 0; i < 4; i++) s_bd.v[i] = sn_value(body);
    sn_heading(body, "THE BOARD'S LAST MESSAGES");
    for (int i = 4; i < 7; i++) s_bd.v[i] = sn_note(body);
    sn_action(HID_LETTER('I'), "I", "ASK IT TO INTRODUCE ITSELF", diag_hello);
    sn_action(HID_LETTER('P'), "P", "PING IT", diag_ping);
    sn_help("For when the board misbehaves. INTRODUCE makes it send its firmware version and what it has on board again; "
            "PING asks for a plain answer. Both answers show under THE BOARD'S LAST MESSAGES.\n\n"
            "DAMAGED counts messages that arrived with a bad checksum. A few after a restart are normal; a steady climb "
            "means a bad connection.");
    diag_render();
}

/* ---- The menu ---------------------------------------------------------------------- */

static void menu_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    const char *problem = sn_board_problem(st);
    if (problem) {
        snprintf(line, n, "%s", problem);
        *level = SN_WARNING;
        return;
    }
    bool whole = st->cc_ok && st->nrf_ok && st->gps_ok && st->wifi_ok;
    snprintf(line, n, "%s\n%s", whole ? "ON AND LINKED" : "A PART IS NOT ANSWERING", st->fix ? "GPS FIX" : "NO GPS FIX YET");
    *level = whole ? SN_NOTE : SN_WARNING;
}

static void gps_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)level;
    if (st->fix) snprintf(line, n, "FIX, %d SATELLITES", st->sats_used);
    else snprintf(line, n, "NO FIX YET, %d IN VIEW", st->sats_view);
}

static const sn_screen_t s_board = {.name = "RADIO BOARD", .purpose = "Is the radio board on, linked and whole? Switch it on or off.", .quote = "1.21 gigawatts!", /* Back to the Future */ .icon = ICON_CHIP, .build = board_build, .destroy = end};
static const sn_screen_t s_gps   = {.name = "GPS", .purpose = "Do you have a position fix, and where are you?", .quote = "Roads? Where we're going, we don't need roads.", /* Back to the Future */ .icon = ICON_PIN, .flags = SN_BOARD, .build = gps_build, .destroy = end, .status = gps_status};
static const sn_screen_t s_diag  = {.name = "DIAGNOSTICS", .purpose = "The link's counters and test commands, for when it misbehaves.", .quote = "I'm sorry, Dave. I'm afraid I can't do that.", /* 2001: A Space Odyssey */ .icon = ICON_TOOLS, .flags = SN_BOARD, .build = diag_build, .destroy = end};

static const sn_screen_t *const s_kids[] = {&s_board, &s_gps, &s_diag};

const sn_screen_t sn_board_menu = {
    .name    = "BOARD",
    .purpose = "The radio board that does the listening, and its GPS.", .quote = "Fist my bump.", /* Project Hail Mary */
    .icon    = ICON_CHIP,
    .kids    = s_kids,
    .nkids   = 3,
    .status  = menu_status,
};
