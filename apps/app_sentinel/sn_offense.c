/*
 * SENTINEL > OFFENSIVE: the tools that transmit. Entering this area asks
 * first and takes the board's arm lease (sn_frame.c); leaving it, by any
 * route, gives the lease back. The board checks the lease itself before it
 * transmits anything: these screens cannot talk it into it.
 *
 *   HONEYPOT  an open access point with a name you choose and nothing
 *             behind it, to see which devices would join
 *
 * The targeted tests of the suite's last phase will live here too.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_notice.h"
#include "deck_theme.h"

#define TICK_MS 500

static const uint8_t s_channels[] = {1, 6, 11, 36, 44, 149, 157};

static struct {
    lv_obj_t   *state, *v[4], *joins;
    lv_timer_t *timer;
    bool        wanted, seen_on; /* seen_on: the board confirmed it was on air */
    uint8_t     ch;
    char        ssid[SENTINEL_WIFI_SSID_MAX + 1];
    sentinel_state_t st;
} s_hp DECK_PSRAM_BSS;

static void toggle(void);
static void change_name(void);
static void next_channel(void);

static void render(void)
{
    const sentinel_state_t *st = &s_hp.st;
    bool on = !strcmp(st->mode, "hpot");
    if (on) {
        sn_set(s_hp.state, g_pal.danger, "!! ON AIR");
        sn_set(s_hp.v[0], g_pal.text, "Transmitting as \"%s\" on channel %u.", s_hp.ssid, s_hp.ch);
        sn_set(s_hp.joins, lv_color_white(), "%u", st->hpot_joins);
        sn_set(s_hp.v[1], g_pal.text, "DEVICES THAT JOINED.   %u JOINED NOW.", st->hpot_stations);
        sn_set(s_hp.v[2], g_pal.label, "Each join is a warning in the LOG, with the device's address.");
    } else {
        if (!s_hp.wanted) sn_set(s_hp.state, g_pal.text, "OFF AIR");
        else sn_set(s_hp.state, g_pal.warn, sentinel_armed(st) ? "STARTING..." : "WAITING FOR THE BOARD TO ARM");
        sn_set(s_hp.v[0], g_pal.text, "Will transmit as \"%s\" on channel %u.", s_hp.ssid, s_hp.ch);
        sn_set(s_hp.joins, g_pal.label, "-");
        sn_set(s_hp.v[1], g_pal.label, "DEVICES THAT JOINED.");
        sn_set(s_hp.v[2], g_pal.label, "Nothing is being transmitted.");
    }
    deck_notice_set_flag(on ? "ON AIR" : "OFFENSIVE");
    /* It went off the air without being asked to here: the board's lease ran out. Stop wanting it. */
    if (on) s_hp.seen_on = true;
    if (s_hp.wanted && s_hp.seen_on && !on) {
        s_hp.wanted = s_hp.seen_on = false;
        sentinel_tool("wifi");
    }
    sn_action_label(toggle, s_hp.wanted ? "TAKE IT OFF AIR" : "GO ON AIR");
    sn_action_danger(toggle, !s_hp.wanted);
    sn_action_enable(change_name, !s_hp.wanted);
    sn_action_enable(next_channel, !s_hp.wanted);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_hp.st);
    render();
}

static void toggle(void)
{
    s_hp.wanted  = !s_hp.wanted;
    s_hp.seen_on = false;
    if (s_hp.wanted) sentinel_honeypot(s_hp.ch, s_hp.ssid);
    else sentinel_tool("wifi");
    render();
}

static void name_entered(const char *text, void *user)
{
    (void)user;
    if (!text || !s_hp.timer) return; /* cancelled, or the screen went while the dialog was up */
    size_t n = strlen(text);
    if (n == 0 || n > SENTINEL_WIFI_SSID_MAX) return;
    snprintf(s_hp.ssid, sizeof(s_hp.ssid), "%s", text);
    hal_cfg_set_str("rf_hp_name", s_hp.ssid);
    render();
}

static void change_name(void)
{
    if (!s_hp.wanted) deck_modal_prompt("THE NETWORK NAME TO TRANSMIT (1 TO 32 CHARACTERS)", s_hp.ssid, name_entered, NULL);
}

static void next_channel(void)
{
    if (s_hp.wanted) return;
    size_t i = 0, count = sizeof(s_channels) / sizeof(s_channels[0]);
    while (i < count && s_channels[i] != s_hp.ch) i++;
    s_hp.ch = s_channels[(i + 1) % count];
    hal_cfg_set_i32("rf_hp_ch", s_hp.ch);
    render();
}

static void build(lv_obj_t *body)
{
    memset(&s_hp, 0, sizeof(s_hp));
    if (!hal_cfg_get_str("rf_hp_name", s_hp.ssid, sizeof(s_hp.ssid)) || !s_hp.ssid[0]) snprintf(s_hp.ssid, sizeof(s_hp.ssid), "FreeWiFi");
    s_hp.ch = (uint8_t)hal_cfg_get_i32("rf_hp_ch", 6);
    if (sentinel_wifi_slot(s_hp.ch) < 0) s_hp.ch = 6;
    sentinel_tool("wifi");
    sentinel_snapshot(&s_hp.st);

    s_hp.state = sn_state(body);
    s_hp.v[0]  = sn_value(body);
    s_hp.joins = lv_label_create(body);
    lv_obj_set_style_text_font(s_hp.joins, g_font.disp_xl, 0);
    lv_obj_set_style_margin_top(s_hp.joins, 14, 0);
    lv_label_set_text(s_hp.joins, "-");
    s_hp.v[1] = sn_value(body);
    s_hp.v[2] = sn_note(body);
    sn_action(HID_ENTER, "ENTER", "GO ON AIR", toggle);
    sn_action(HID_LETTER('N'), "N", "CHANGE THE NAME", change_name);
    sn_action(HID_LETTER('C'), "C", "NEXT CHANNEL", next_channel);
    sn_help("Devices join networks they remember by name alone. An impostor only has to use the name.\n\n"
            "The honeypot is that impostor, harmless: open, no address handed out, no traffic carried. What it tells you "
            "is which devices would have followed. Use a name your own devices know.\n\n"
            "It also gives your own detectors something to see: a new open network, and with a name you already use, a "
            "possible impostor in WI-FI > NETWORKS.\n\n"
            "Only on names and in places that are yours to test. Leaving this screen takes it off the air.");
    s_hp.timer = lv_timer_create(tick, TICK_MS, NULL);
    render();
}

static void destroy(void)
{
    if (s_hp.timer) lv_timer_delete(s_hp.timer);
    s_hp.timer  = NULL;
    s_hp.wanted = false;
    sentinel_tool("watch"); /* off the air: it is not something to forget running */
    if (sn_mode() == SN_OFFENSIVE) deck_notice_set_flag("OFFENSIVE");
}

static void menu_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)st;
    (void)level;
    snprintf(line, n, "%s", sn_mode() == SN_OFFENSIVE ? "YOU ARE IN OFFENSIVE MODE" : "TOOLS THAT TRANSMIT.\nASKS BEFORE ENTERING.");
}

static const sn_screen_t s_honeypot = {
    .name    = "HONEYPOT",
    .purpose = "Which devices would join a network with this name?", .quote = "It's a trap!", /* Return of the Jedi */
    .icon    = ICON_WIFI,
    .flags   = SN_BOARD,
    .build   = build,
    .destroy = destroy,
};

static const sn_screen_t *const s_kids[] = {&s_honeypot};

const sn_screen_t sn_offense_menu = {
    .name    = "OFFENSIVE",
    .purpose = "Tools that transmit. Only on equipment that is yours to test.", .quote = "Shiny. Let's be bad guys.", /* Serenity */
    .icon    = ICON_BOLT,
    .flags   = SN_OFFENSE,
    .kids    = s_kids,
    .nkids   = 1,
    .status  = menu_status,
};
