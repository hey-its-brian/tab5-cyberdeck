/*
 * SENTINEL > OFFENSIVE: the tools that transmit. Entering this area asks
 * first and takes the board's arm lease (sn_frame.c); leaving it, by any
 * route, gives the lease back. The board checks the lease itself before it
 * transmits anything: these screens cannot talk it into it.
 *
 *   HONEYPOT    an open access point with a name you choose and nothing
 *               behind it, to see which devices would join
 *   DEAUTH TEST one short counted burst at one client of one network, so
 *               that the deauth detector can be seen to fire on something
 *               real instead of resting on its unit tests
 *
 * The deauth test is three pages, one choice each: the network, then the
 * client, then the run. Only the third sends anything, and only after the
 * permission warning has been read and answered. sentinel_test.h holds the
 * rules about what a target may be, and the board applies them again.
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


/* ---- DEAUTH TEST ---------------------------------------------------------- */

#define NET_ROWS 32

typedef enum { PICK_NETWORK, PICK_CLIENT, THE_RUN } step_t;

static struct {
    step_t      step;
    sn_list_t   list;
    lv_obj_t   *state, *summary, *v[5];
    lv_timer_t *timer;
    uint8_t     bssid[6], station[6], ch;
    char        ssid[SENTINEL_WIFI_SSID_MAX + 1];
    bool        asked;       /* a run has been asked for on this page */
    const char *refused;     /* why the rules said no, if they did */
    sentinel_state_t    st;
} s_dt DECK_PSRAM_BSS;

static sentinel_wifihost_t s_dt_wifi DECK_PSRAM_BSS;

static void step_network(void);
static void step_client(void);
static void step_run(void);

static int networks(void) { return s_dt_wifi.n > NET_ROWS ? NET_ROWS : s_dt_wifi.n; }

/* ---- 1. the network ---- */

static void network_render(void)
{
    int n = networks();
    sn_list_set(&s_dt.list, n, s_dt.list.sel, "LISTENING FOR NETWORKS...");
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        int k = sn_list_item(&s_dt.list, r);
        if (k < 0) continue;
        const sentinel_wbs_t *a = &s_dt_wifi.ap[k].d;
        sn_set(sn_list_text(&s_dt.list, r), g_pal.text, "%4d dBm   CH %-3u  %-6s  %s", a->rssi, a->ch,
               sentinel_sec_name(a->sec), a->ssid[0] ? a->ssid : "(hidden)");
    }
    sn_set(s_dt.summary, g_pal.label, "%d NETWORKS IN RANGE. CHOOSE THE ONE YOU ARE TESTING.", n);
}

static void network_chosen(int index)
{
    if (index < 0 || index >= networks()) return;
    const sentinel_wbs_t *a = &s_dt_wifi.ap[index].d;
    memcpy(s_dt.bssid, a->bssid, 6);
    s_dt.ch = a->ch;
    snprintf(s_dt.ssid, sizeof(s_dt.ssid), "%s", a->ssid[0] ? a->ssid : "(hidden)");
    step_client();
}

static void step_network(void)
{
    lv_obj_t *body = sn_body_reset();
    s_dt.step      = PICK_NETWORK;
    s_dt.state     = NULL;
    sentinel_stations(NULL, 0); /* back to hearing every channel */
    sentinel_tool("wifi");
    s_dt.summary = sn_note(body);
    sn_list_build(&s_dt.list, body, network_chosen);
    sn_help("The first of three choices. Nothing is sent on this page or the next one.\n\n"
            "Choose the network you are testing. It has to be one you own, or one whose owner knows and has agreed to "
            "this; you will be asked to confirm that before anything goes out.");
    network_render();
}

/* ---- 2. the client ---- */

static void client_render(void)
{
    const sentinel_state_t *st = &s_dt.st;
    int n = st->sta_n;
    sn_list_set(&s_dt.list, n, s_dt.list.sel, "NOTHING HEARD ON THIS NETWORK YET");
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        int k = sn_list_item(&s_dt.list, r);
        if (k < 0) continue;
        const sentinel_sta_t *c = &st->sta[k];
        char mac[18], ago[24];
        sentinel_mac_text(mac, c->mac);
        sn_fmt_span(ago, sizeof(ago), c->age_ms);
        sn_set(sn_list_text(&s_dt.list, r), c->age_ms > 5000 ? g_pal.label : g_pal.text, "%4d dBm   %s   %u FRAMES   %s AGO",
               c->rssi, mac, (unsigned)c->frames, ago);
    }
    if (n) sn_set(s_dt.summary, g_pal.label, "%d HEARD ON \"%s\", CHANNEL %u. CHOOSE ONE.", n, s_dt.ssid, s_dt.ch);
    else
        sn_set(s_dt.summary, g_pal.label, "LISTENING ON \"%s\", CHANNEL %u. A CLIENT THAT SAYS NOTHING IS NOT HEARD.",
               s_dt.ssid, s_dt.ch);
}

static void client_chosen(int index)
{
    if (index < 0 || index >= s_dt.st.sta_n) return;
    memcpy(s_dt.station, s_dt.st.sta[index].mac, 6);
    step_run();
}

static void step_client(void)
{
    lv_obj_t *body = sn_body_reset();
    s_dt.step      = PICK_CLIENT;
    s_dt.state     = NULL;
    memset(&s_dt.list, 0, sizeof(s_dt.list));
    sentinel_stations(s_dt.bssid, s_dt.ch); /* park there and listen */
    s_dt.summary = sn_note(body);
    sn_list_build(&s_dt.list, body, client_chosen);
    sn_help("The board is parked on this network's channel, listening for frames between it and its clients. Every "
            "address it hears appears here. Nothing is sent.\n\n"
            "A client that is connected but silent will not appear until it says something. A phone usually does within "
            "a few seconds; something idle may take longer.\n\n"
            "The test goes to the one client you choose here, and to nothing else.");
    client_render();
}

/* ---- 3. the run ---- */

static void run_now(void)
{
    sentinel_deauth_t t;
    memcpy(t.bssid, s_dt.bssid, 6);
    memcpy(t.station, s_dt.station, 6);
    t.channel = s_dt.ch;
    s_dt.asked = sentinel_deauth(&t, &s_dt.refused);
}

static void run_ask(void)
{
    static char message[400];
    char        sta[18];
    sentinel_mac_text(sta, s_dt.station);
    /* The permission sentence, every run, with the target spelled out. */
    snprintf(message, sizeof(message),
             "This sends %d deauthentication frames to %s on \"%s\", channel %u, over about %d ms.\n\n"
             "That network must be your own, or one whose owner knows about this test and has agreed to it.\n\n"
             "Nothing else is affected: no other client, and no other network.",
             SENTINEL_DEAUTH_BURST, sta, s_dt.ssid, s_dt.ch, SENTINEL_DEAUTH_BURST * SENTINEL_DEAUTH_GAP_MS);
    sn_confirm("IS THIS NETWORK YOURS TO TEST?", message, "SEND THE BURST", run_now);
}

static void run_render(void)
{
    const sentinel_state_t *st = &s_dt.st;
    char sta[18], ap[18];
    sentinel_mac_text(sta, s_dt.station);
    sentinel_mac_text(ap, s_dt.bssid);

    bool armed = sentinel_armed(st);
    if (s_dt.refused) {
        sn_set(s_dt.state, g_pal.danger, "!! REFUSED");
        sn_set(s_dt.v[0], g_pal.text, "%s", s_dt.refused);
    } else if (st->txd_ms) {
        sn_set(s_dt.state, g_pal.ok, "SENT %u OF %u", st->txd_sent, st->txd_asked);
        sn_set(s_dt.v[0], g_pal.text, "Look in the LOG: the deauth detector should have a row of its own from the same moment.");
    } else if (s_dt.asked) {
        sn_set(s_dt.state, g_pal.accent, "SENDING...");
        sn_set(s_dt.v[0], g_pal.label, "About a sixth of a second.");
    } else if (!armed) {
        sn_set(s_dt.state, g_pal.warn, "! THE BOARD IS NOT ARMED YET");
        sn_set(s_dt.v[0], g_pal.text, "The board holds the lease only while SENTINEL keeps asking. A moment.");
    } else {
        sn_set(s_dt.state, g_pal.text, "READY");
        sn_set(s_dt.v[0], g_pal.text, "%d frames over about %d ms, to that one client and nothing else.",
               SENTINEL_DEAUTH_BURST, SENTINEL_DEAUTH_BURST * SENTINEL_DEAUTH_GAP_MS);
    }
    sn_set(s_dt.v[1], lv_color_white(), "CLIENT    %s", sta);
    sn_set(s_dt.v[2], g_pal.text, "NETWORK   %s  (%s)", s_dt.ssid, ap);
    sn_set(s_dt.v[3], g_pal.text, "CHANNEL   %u", s_dt.ch);
    sn_set(s_dt.v[4], g_pal.label,
           "It must be your own network, or one whose owner knows and has agreed. You will be asked to confirm that.");
    sn_action_enable(run_ask, armed);
    sn_action_label(run_ask, st->txd_ms || s_dt.refused ? "RUN IT AGAIN" : "RUN THE TEST");
}

static void step_run(void)
{
    lv_obj_t *body = sn_body_reset();
    s_dt.step      = THE_RUN;
    s_dt.asked     = false;
    s_dt.refused   = NULL;
    memset(&s_dt.list, 0, sizeof(s_dt.list));
    s_dt.state = sn_state(body);
    s_dt.v[0]  = sn_value(body);
    sn_heading(body, "WHAT WILL BE SENT, AND TO WHOM");
    for (int i = 1; i < 4; i++) s_dt.v[i] = sn_value(body);
    s_dt.v[4] = sn_note(body);
    sn_action(HID_ENTER, "ENTER", "RUN THE TEST", run_ask);
    sn_action_danger(run_ask, true);
    sn_help("One run is a short burst of deauthentication frames addressed from the access point to the one client you "
            "chose, and then it is over. There is no continuous mode.\n\n"
            "It is built to be seen rather than to disconnect: only the half of the usual pair that a detector reads is "
            "sent. A client may stay connected, and the test has still done its job if your own detector noticed.\n\n"
            "Every run is written to the log with its target. The board checks the rules and the arm lease again for "
            "itself, so a screen cannot talk it into anything.");
    run_render();
}

static void deauth_tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_dt.st);
    if (s_dt.step == PICK_NETWORK) {
        sentinel_wifi_copy(&s_dt_wifi);
        network_render();
    } else if (s_dt.step == PICK_CLIENT) {
        client_render();
    } else {
        run_render();
    }
}

static void deauth_build(lv_obj_t *body)
{
    (void)body;
    memset(&s_dt, 0, sizeof(s_dt));
    sentinel_wifihost_init(&s_dt_wifi);
    sentinel_snapshot(&s_dt.st);
    sentinel_wifi_copy(&s_dt_wifi);
    step_network();
    s_dt.timer = lv_timer_create(deauth_tick, TICK_MS, NULL);
}

static void deauth_destroy(void)
{
    if (s_dt.timer) lv_timer_delete(s_dt.timer);
    s_dt.timer = NULL;
    memset(&s_dt.list, 0, sizeof(s_dt.list));
    sentinel_stations(NULL, 0);
    sentinel_tool("watch");
}

/* BACK walks out of the test the way it walked in. */
static bool deauth_back(void)
{
    if (s_dt.step == THE_RUN) {
        step_client();
        return true;
    }
    if (s_dt.step == PICK_CLIENT) {
        step_network();
        return true;
    }
    return false;
}

static bool deauth_key(const deck_key_t *k)
{
    step_t was = s_dt.step;
    if (was == THE_RUN || !sn_list_key(&s_dt.list, k)) return false;
    /* Enter on a row moves to the next page, and that page has already
     * drawn itself: the list these would redraw is gone. */
    if (s_dt.step != was) return true;
    if (was == PICK_NETWORK) network_render();
    else client_render();
    return true;
}

static void deauth_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)level;
    if (!st->tst_ok) snprintf(line, n, "NEEDS BOARD FIRMWARE 0.8.0");
    else snprintf(line, n, "ONE CLIENT, ONE NETWORK, ONE SHORT BURST.");
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

static const sn_screen_t s_deauth = {
    .name    = "DEAUTH TEST",
    .purpose = "Make your own deauth detector fire, on one client you choose.",
    .quote   = "I'm going to allow that.", /* The Big Lebowski */
    .icon    = ICON_BOLT,
    .flags   = SN_BOARD,
    .build   = deauth_build,
    .destroy = deauth_destroy,
    .key     = deauth_key,
    .back    = deauth_back,
    .status  = deauth_status,
};

static const sn_screen_t *const s_kids[] = {&s_honeypot, &s_deauth};

const sn_screen_t sn_offense_menu = {
    .name    = "OFFENSIVE",
    .purpose = "Tools that transmit. Only on equipment that is yours to test.", .quote = "Shiny. Let's be bad guys.", /* Serenity */
    .icon    = ICON_BOLT,
    .flags   = SN_OFFENSE,
    .kids    = s_kids,
    .nkids   = 2,
    .status  = menu_status,
};
