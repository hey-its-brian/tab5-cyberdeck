/*
 * RF WATCH > WI-FI: what is on the air around the deck. While this screen is
 * up the C5 hops all 38 channels and reports what it hears; nothing is sent.
 * Four views, on keys 1 to 4:
 *
 *   MONITOR   a waterfall, one column per channel, newest sweep on top, with
 *             the latest sweep as bars above it. Red means deauthentication
 *             frames were heard there.
 *   NETWORKS  every access point with its channel, security and signal, and
 *             which of channels 1, 6 and 11 is the quietest.
 *   DRONES    aircraft broadcasting Remote ID.
 *   HONEYPOT  an open access point with a name you choose and nothing behind
 *             it, to see who joins. It transmits, so it only exists in a RED
 *             TEAM session; the node checks the arm lease, not this screen.
 *
 * The detectors (deauth flood, evil twin, beacon flood, karma, drones) run
 * on the node all the time and reach the user as notices; this screen is
 * for looking. See sentinel_wifimon.h for what each one means.
 */
#include "rf_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_modal.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "sentinel_cmd.h"

#define TICK_MS  500
#define ROWS     14
#define BARS_H   78 /* the strip of bars above the waterfall */
#define BAND_GAP 12 /* between the 2.4 GHz and 5 GHz columns */

typedef enum { VIEW_MONITOR, VIEW_NETWORKS, VIEW_DRONES, VIEW_HONEYPOT, VIEW_N } wifi_view_t;
static const char *const s_view_name[VIEW_N] = {"1 MONITOR", "2 NETWORKS", "3 DRONES", "4 HONEYPOT"};
static const uint8_t s_hp_channels[] = {1, 6, 11, 36, 44, 149, 157};

static struct {
    lv_obj_t *tab[VIEW_N], *content, *hint;
    lv_obj_t *line[8];     /* the left panel's lines, whatever the view */
    lv_obj_t *big, *chart; /* MONITOR: the network count, the waterfall */
    lv_obj_t *row[ROWS];   /* NETWORKS and DRONES */
    lv_timer_t *timer;
    wifi_view_t view;
    int      sel_slot;     /* MONITOR: the channel under the cursor */
    int      sel_index;    /* NETWORKS: the row */
    bool     cap_wanted, hp_wanted;
    uint32_t cap_base, lost_base;
    uint8_t  hp_ch;
    char     hp_ssid[SENTINEL_WIFI_SSID_MAX + 1];
    char     ch_text[SENTINEL_WIFI_CHANNELS][4];
    sentinel_state_t    st;
    sentinel_wifihost_t w;
} s_wf DECK_PSRAM_BSS;

static void set_view(wifi_view_t v);

/* ---- Small helpers -------------------------------------------------------- */

static void fmt_span(char *out, size_t n, uint32_t ms)
{
    unsigned s = (unsigned)(ms / 1000);
    if (s < 60) snprintf(out, n, "%u S", s);
    else if (s < 3600) snprintf(out, n, "%u MIN", s / 60);
    else snprintf(out, n, "%u H %u MIN", s / 3600, s / 60 % 60);
}

/* Another access point carries this name with the opposite kind of
 * security: one of the pair is an impostor. */
static bool has_twin(const sentinel_wbs_t *a)
{
    if (!a->ssid[0]) return false;
    for (int i = 0; i < s_wf.w.n; i++) {
        const sentinel_wbs_t *o = &s_wf.w.ap[i].d;
        if (o != a && !strcmp(o->ssid, a->ssid) && (o->sec == SENTINEL_SEC_OPEN) != (a->sec == SENTINEL_SEC_OPEN)) return true;
    }
    return false;
}

static lv_color_t ap_color(const sentinel_wbs_t *a)
{
    if (has_twin(a) && a->sec == SENTINEL_SEC_OPEN) return g_pal.danger; /* the open one of the pair is the likely impostor */
    if (a->sec <= SENTINEL_SEC_WPA) return g_pal.warn; /* open, WEP and WPA are all weak today */
    return a->ssid[0] ? g_pal.text : g_pal.dim;
}

static const char *radio_line(void)
{
    const sentinel_state_t *st = &s_wf.st;
    if (!sentinel_link(st)) return "WAITING FOR THE C5";
    if (!st->mon_ok) return "THIS NODE HAS NO WI-FI MONITOR (FIRMWARE BEFORE 0.5.0)";
    if (!strcmp(st->mode, "wifi")) return "RADIO: HOPPING 38 CHANNELS, BLUETOOTH WAITING";
    if (!strcmp(st->mode, "hpot")) return "RADIO: HONEYPOT ON AIR";
    return "RADIO: ASKING FOR THE WI-FI TOOL";
}

static void capture_line(lv_obj_t *label)
{
    const sentinel_state_t *st = &s_wf.st;
    sentinel_store_stats_t  log;
    sentinel_stats(&log);
    if (st->capture) {
        unsigned lost = (unsigned)(log.packets_dropped - s_wf.lost_base);
        if (lost) lv_label_set_text_fmt(label, "CAPTURE ON  ::  %u FRAMES TO SD, %u LOST", (unsigned)(log.packets - s_wf.cap_base), lost);
        else lv_label_set_text_fmt(label, "CAPTURE ON  ::  %u FRAMES TO SD", (unsigned)(log.packets - s_wf.cap_base));
        lv_obj_set_style_text_color(label, lost ? g_pal.warn : g_pal.accent, 0);
        return;
    }
    s_wf.cap_base  = log.packets;
    s_wf.lost_base = log.packets_dropped;
    if (s_wf.cap_wanted && st->host_baud < SENTINEL_CAP_MIN_BAUD)
        lv_label_set_text_fmt(label, "CAPTURE NEEDS THE FAST LINK (NOW %u BAUD)", (unsigned)st->host_baud);
    else lv_label_set_text(label, s_wf.cap_wanted ? "CAPTURE: ASKING THE C5" : "CAPTURE OFF");
    lv_obj_set_style_text_color(label, s_wf.cap_wanted ? g_pal.warn : g_pal.dim, 0);
}

/* ---- MONITOR: the waterfall ----------------------------------------------- */

static lv_color_t heat(uint8_t v)
{
    if (v == SENTINEL_WATERFALL_ALARM) return g_pal.danger;
    if (v == 0) return g_pal.panel;
    /* dark to the accent, then towards white for only the very busiest */
    if (v < 210) return lv_color_mix(g_pal.accent, g_pal.panel, (uint8_t)(12 + v * 243 / 210));
    return lv_color_mix(lv_color_white(), g_pal.accent, (uint8_t)((v - 210) * 5));
}

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa   = LV_OPA_COVER;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

static void chart_draw(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t cw = (w - BAND_GAP) / SENTINEL_WIFI_CHANNELS; /* a column, with a 1 px gutter inside it */
    int32_t label_h = 20, fall_y = a.y1 + BARS_H + label_h + 6;
    int32_t rh = (a.y2 - fall_y) / SENTINEL_WATERFALL_ROWS;
    if (cw < 3 || rh < 2 || h < BARS_H + label_h + 40) return;

    for (int s = 0; s < SENTINEL_WIFI_CHANNELS; s++) {
        int32_t x = a.x1 + s * cw + (s >= 13 ? BAND_GAP : 0);
        /* the latest dwell as a bar */
        const sentinel_wch_t *c = &s_wf.w.ch[s];
        uint8_t v  = c->ms ? sentinel_wifi_intensity(c) : 0;
        int32_t bh = v == SENTINEL_WATERFALL_ALARM ? BARS_H : (int32_t)v * BARS_H / 254;
        fill(layer, x, a.y1, x + cw - 2, a.y1 + BARS_H, s == s_wf.sel_slot ? g_pal.panel_hi : g_pal.panel);
        if (bh > 0) fill(layer, x, a.y1 + BARS_H - bh, x + cw - 2, a.y1 + BARS_H, heat(v));
        /* channel numbers, where they fit: every one on 2.4 GHz would collide */
        uint8_t ch = sentinel_wifi_channel[s];
        if (s == s_wf.sel_slot || ch == 1 || ch == 6 || ch == 11 || ch == 36 || ch == 100 || ch == 149 || ch == 165) {
            lv_draw_label_dsc_t t;
            lv_draw_label_dsc_init(&t);
            t.text  = s_wf.ch_text[s];
            t.font  = g_font.mono_s;
            t.color = s == s_wf.sel_slot ? g_pal.text : g_pal.dim;
            lv_area_t ta = {x, a.y1 + BARS_H + 3, x + cw + 24, a.y1 + BARS_H + 3 + label_h};
            if (s == s_wf.sel_slot || s_wf.sel_slot < 0 || (s - s_wf.sel_slot > 2 || s_wf.sel_slot - s > 2)) lv_draw_label(layer, &t, &ta);
        }
        /* the history under it, newest first */
        for (int r = 0; r < SENTINEL_WATERFALL_ROWS; r++) {
            const uint8_t *row = sentinel_wifihost_row(&s_wf.w, r);
            if (!row) break;
            int32_t y = fall_y + r * rh;
            fill(layer, x, y, x + cw - 2, y + rh - 2, heat(row[s]));
        }
    }
}

static void render_monitor(void)
{
    const sentinel_wifihost_t *w = &s_wf.w;
    int busiest = -1, deauths = 0;
    uint32_t frames = 0, ms = 0;
    for (int s = 0; s < SENTINEL_WIFI_CHANNELS; s++) {
        const sentinel_wch_t *c = &w->ch[s];
        if (!c->ms) continue;
        frames += (uint32_t)c->mgmt + c->data + c->ctrl;
        ms += c->ms;
        deauths += c->deauths;
        if (busiest < 0 || sentinel_wifi_intensity(c) > sentinel_wifi_intensity(&w->ch[busiest])) busiest = s;
    }
    lv_label_set_text_fmt(s_wf.big, "%d", w->n);
    lv_label_set_text_fmt(s_wf.line[0], "SWEEPS %u    %u FRAMES/S HEARD", (unsigned)w->sweeps, ms ? (unsigned)(frames * 1000u / ms) : 0u);
    if (busiest >= 0) lv_label_set_text_fmt(s_wf.line[1], "BUSIEST: CHANNEL %u", sentinel_wifi_channel[busiest]);
    else lv_label_set_text(s_wf.line[1], "NO SWEEP YET");
    lv_label_set_text_fmt(s_wf.line[2], "DEAUTH FRAMES IN THE LAST SWEEP: %d", deauths);
    lv_obj_set_style_text_color(s_wf.line[2], deauths ? g_pal.danger : g_pal.dim, 0);
    lv_label_set_text(s_wf.line[3], radio_line());
    capture_line(s_wf.line[4]);

    const sentinel_wch_t *c = &w->ch[s_wf.sel_slot];
    uint8_t ch = sentinel_wifi_channel[s_wf.sel_slot];
    int8_t  best;
    int     aps = sentinel_wifihost_aps_on(w, ch, &best);
    unsigned mhz = ch <= 13 ? 2407u + 5u * ch : 5000u + 5u * ch;
    lv_label_set_text_fmt(s_wf.line[5], "CHANNEL %u    %u MHZ", ch, mhz);
    if (aps) lv_label_set_text_fmt(s_wf.line[6], "%d NETWORK%s, STRONGEST %d dBm", aps, aps == 1 ? "" : "S", best);
    else lv_label_set_text(s_wf.line[6], "NO NETWORKS HEARD HERE");
    if (c->ms) lv_label_set_text_fmt(s_wf.line[7], "IN %u MS: %u BEACONS  %u PROBES  %u DATA  %u CONTROL  %u DEAUTH", c->ms, c->beacons, c->probes, c->data, c->ctrl, c->deauths);
    else lv_label_set_text(s_wf.line[7], "NOT HEARD YET");
    lv_obj_set_style_text_color(s_wf.line[7], c->deauths ? g_pal.danger : g_pal.dim, 0);
    lv_obj_invalidate(s_wf.chart);
}

/* ---- NETWORKS ------------------------------------------------------------- */

static void render_networks(void)
{
    const sentinel_wifihost_t *w = &s_wf.w;
    int n = w->n, sel = s_wf.sel_index;
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    s_wf.sel_index = sel;
    int top = sel - ROWS / 2;
    if (top > n - ROWS) top = n - ROWS;
    if (top < 0) top = 0;
    for (int r = 0; r < ROWS; r++) {
        lv_obj_t *row = s_wf.row[r];
        int k = top + r;
        if (k >= n) {
            lv_label_set_text(row, r == 0 && n == 0 ? "LISTENING..." : "");
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(row, g_pal.dim, 0);
            continue;
        }
        const sentinel_wbs_t *a = &w->ap[k].d;
        char mac[18];
        sentinel_mac_text(mac, a->bssid);
        lv_label_set_text_fmt(row, "%4d  CH %-3u  %-4s%s  %s  %s", a->rssi, a->ch, sentinel_sec_name(a->sec), a->pmf ? "+PMF" : "    ", mac,
                              a->ssid[0] ? a->ssid : "(hidden)");
        lv_obj_set_style_text_color(row, ap_color(a), 0);
        lv_obj_set_style_bg_opa(row, k == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }

    /* The channel analyzer's one-line answer: which of the three channels
     * that do not overlap has the fewest neighbours. */
    static const uint8_t pick[3] = {1, 6, 11};
    int best = 0, counts[3];
    for (int i = 0; i < 3; i++) {
        counts[i] = 0;
        for (int k = 0; k < n; k++) {
            int d = (int)w->ap[k].d.ch - pick[i];
            if (w->ap[k].d.ch <= 13 && d >= -2 && d <= 2) counts[i]++; /* a neighbour two channels over still overlaps */
        }
        if (counts[i] < counts[best]) best = i;
    }
    int open = 0, weak = 0, twins = 0;
    for (int k = 0; k < n; k++) {
        if (w->ap[k].d.sec == SENTINEL_SEC_OPEN) open++;
        else if (w->ap[k].d.sec <= SENTINEL_SEC_WPA) weak++;
        if (has_twin(&w->ap[k].d) && w->ap[k].d.sec == SENTINEL_SEC_OPEN) twins++;
    }
    lv_label_set_text_fmt(s_wf.big, "%d", n);
    lv_label_set_text_fmt(s_wf.line[0], "OPEN %d    WEP OR WPA %d    NAME CLASHES %d", open, weak, twins);
    lv_obj_set_style_text_color(s_wf.line[0], twins ? g_pal.danger : (open || weak) ? g_pal.warn : g_pal.dim, 0);
    lv_label_set_text_fmt(s_wf.line[1], "2.4 GHZ: CH 1 HAS %d, CH 6 HAS %d, CH 11 HAS %d", counts[0], counts[1], counts[2]);
    lv_label_set_text_fmt(s_wf.line[2], "QUIETEST OF THE THREE: CHANNEL %u", pick[best]);
    lv_label_set_text(s_wf.line[3], radio_line());
    capture_line(s_wf.line[4]);
    if (n == 0) {
        lv_label_set_text(s_wf.line[5], "");
        lv_label_set_text(s_wf.line[6], "");
        lv_label_set_text(s_wf.line[7], "");
        return;
    }
    const sentinel_wifitab_ap_t *e = &w->ap[sel];
    char span[24];
    fmt_span(span, sizeof(span), sentinel_now_ms() - e->first_ms);
    lv_label_set_text_fmt(s_wf.line[5], "%s", e->d.ssid[0] ? e->d.ssid : "(HIDDEN NAME)");
    lv_obj_set_style_text_color(s_wf.line[5], ap_color(&e->d), 0);
    lv_label_set_text_fmt(s_wf.line[6], "%s%s    %u BEACONS    LISTED %s", sentinel_sec_name(e->d.sec),
                          e->d.pmf ? ", MANAGEMENT FRAMES PROTECTED" : e->d.sec >= SENTINEL_SEC_WPA2 ? ", DEAUTH CAN BE FORGED" : "", e->d.beacons, span);
    if (has_twin(&e->d) && e->d.sec == SENTINEL_SEC_OPEN) lv_label_set_text(s_wf.line[7], "THIS NAME IS PROTECTED ON ANOTHER ACCESS POINT AND OPEN HERE: THIS ONE MAY BE AN IMPOSTOR");
    else if (has_twin(&e->d)) lv_label_set_text(s_wf.line[7], "AN OPEN COPY OF THIS NAME IS NEARBY: DEVICES MAY JOIN THAT ONE INSTEAD");
    else if (e->d.sec == SENTINEL_SEC_OPEN) lv_label_set_text(s_wf.line[7], "OPEN: ANYONE NEARBY CAN READ WHAT IS NOT ENCRYPTED ANOTHER WAY");
    else lv_label_set_text(s_wf.line[7], "");
    lv_obj_set_style_text_color(s_wf.line[7], has_twin(&e->d) ? g_pal.danger : g_pal.warn, 0);
}

/* ---- DRONES --------------------------------------------------------------- */

static void render_drones(void)
{
    int n = 0;
    uint32_t now = sentinel_now_ms();
    for (int i = 0; i < SENTINEL_DRONES_MAX && n < ROWS; i++) {
        const sentinel_drone_t *d = &s_wf.w.drone[i];
        if (!d->used) continue;
        char mac[18], span[24];
        sentinel_mac_text(mac, d->d.mac);
        fmt_span(span, sizeof(span), now - d->last_ms);
        if (d->d.lat != 0 || d->d.lon != 0)
            lv_label_set_text_fmt(s_wf.row[n], "%4d  %-20s %.5f, %.5f  %4d M  %s  %s", d->d.rssi, d->d.id[0] ? d->d.id : mac, d->d.lat, d->d.lon, (int)d->d.alt_m,
                                  d->d.ble ? "BLE " : "WIFI", span);
        else lv_label_set_text_fmt(s_wf.row[n], "%4d  %-20s NO POSITION GIVEN          %s  %s", d->d.rssi, d->d.id[0] ? d->d.id : mac, d->d.ble ? "BLE " : "WIFI", span);
        lv_obj_set_style_text_color(s_wf.row[n], now - d->last_ms < 15000 ? g_pal.warn : g_pal.dim, 0);
        n++;
    }
    for (int r = n; r < ROWS; r++) {
        lv_label_set_text(s_wf.row[r], r == 0 ? "NO REMOTE ID HEARD" : "");
        lv_obj_set_style_text_color(s_wf.row[r], g_pal.dim, 0);
    }
    lv_label_set_text_fmt(s_wf.big, "%d", n);
    lv_label_set_text(s_wf.line[0], "AIRCRAFT BROADCASTING REMOTE ID, ON WI-FI OR BLUETOOTH.");
    lv_label_set_text(s_wf.line[1], "THE ID AND POSITION ARE WHAT THE AIRCRAFT CLAIMS.");
    lv_label_set_text(s_wf.line[2], "A DRONE THAT BROADCASTS NOTHING IS NOT SEEN.");
    lv_label_set_text(s_wf.line[3], radio_line());
    lv_label_set_text(s_wf.line[4], s_wf.st.fix ? "" : "NO GPS FIX: YOUR OWN POSITION IS UNKNOWN");
    lv_label_set_text(s_wf.line[5], "");
    lv_label_set_text(s_wf.line[6], "");
    lv_label_set_text(s_wf.line[7], "");
}

/* ---- HONEYPOT ------------------------------------------------------------- */

static void render_honeypot(void)
{
    const sentinel_state_t *st = &s_wf.st;
    bool red = rf_mode() == RF_MODE_RED, on = !strcmp(st->mode, "hpot");
    lv_label_set_text_fmt(s_wf.big, "%u", on ? st->hpot_joins : 0);
    lv_label_set_text_fmt(s_wf.line[0], "NAME     %s", s_wf.hp_ssid);
    lv_label_set_text_fmt(s_wf.line[1], "CHANNEL  %u", s_wf.hp_ch);
    if (!red) {
        lv_label_set_text(s_wf.line[2], "RED TEAM ONLY");
        lv_obj_set_style_text_color(s_wf.line[2], g_pal.warn, 0);
        lv_label_set_text(s_wf.line[3], "AN ACCESS POINT TRANSMITS, AND A SAFE SESSION TRANSMITS NOTHING. LEAVE RF WATCH AND REOPEN IT IN RED TEAM.");
        lv_label_set_text(s_wf.line[4], "");
    } else if (on) {
        lv_label_set_text(s_wf.line[2], "ON AIR");
        lv_obj_set_style_text_color(s_wf.line[2], g_pal.danger, 0);
        lv_label_set_text_fmt(s_wf.line[3], "%u JOINED NOW    %u JOINS    %u PROBE REQUESTS", st->hpot_stations, st->hpot_joins, st->hpot_probes);
        lv_label_set_text(s_wf.line[4], "EACH JOIN IS A WARNING IN THE LOG, WITH THE DEVICE'S ADDRESS.");
    } else {
        lv_label_set_text(s_wf.line[2], s_wf.hp_wanted ? (sentinel_armed(st) ? "STARTING..." : "WAITING FOR THE NODE TO ARM") : "OFF");
        lv_obj_set_style_text_color(s_wf.line[2], s_wf.hp_wanted ? g_pal.warn : g_pal.dim, 0);
        lv_label_set_text(s_wf.line[3], "AN OPEN ACCESS POINT WITH THIS NAME AND NOTHING BEHIND IT: A DEVICE CAN JOIN AND NO MORE.");
        lv_label_set_text(s_wf.line[4], "USE A NAME YOUR OWN DEVICES KNOW, TO SEE WHICH OF THEM WOULD FOLLOW AN IMPOSTOR.");
    }
    lv_label_set_text(s_wf.line[5], radio_line());
    lv_label_set_text(s_wf.line[6], "");
    lv_label_set_text(s_wf.line[7], "");
    /* The lease went (the session ended, the screen slept): the node has stopped it, so stop wanting it. */
    if (s_wf.hp_wanted && !red) s_wf.hp_wanted = false;
}

/* ---- The screen ----------------------------------------------------------- */

static void render(void)
{
    if (!s_wf.content) return;
    switch (s_wf.view) {
        case VIEW_MONITOR: render_monitor(); break;
        case VIEW_NETWORKS: render_networks(); break;
        case VIEW_DRONES: render_drones(); break;
        default: render_honeypot(); break;
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_wf.st);
    sentinel_wifi_copy(&s_wf.w);
    render();
}

static void tab_clicked(lv_event_t *e) { set_view((wifi_view_t)(intptr_t)lv_event_get_user_data(e)); }

static void set_view(wifi_view_t v)
{
    static const char *const title[VIEW_N] = {"WI-FI MONITOR", "NETWORKS AND CHANNELS", "DRONES  REMOTE ID", "HONEYPOT"};
    static const char *const unit[VIEW_N]  = {"NETWORKS HEARD", "NETWORKS HEARD", "IN EARSHOT", "JOINS"};
    static const char *const hint[VIEW_N]  = {
        "[1-4] VIEW   [LEFT/RIGHT] CHANNEL   [C] CAPTURE TO SD   [ESC] TOOLS      RED: DEAUTH FRAMES HEARD",
        "[1-4] VIEW   [UP/DOWN] SELECT   [C] CAPTURE TO SD   [ESC] TOOLS      YELLOW: WEAK OR NO SECURITY   RED: NAME CLASH",
        "[1-4] VIEW   [ESC] TOOLS",
        "[1-4] VIEW   [N] NAME   [K] CHANNEL   [ENTER] START OR STOP   [ESC] TOOLS",
    };
    /* Leaving the honeypot view takes it off the air: it is not something to forget running. */
    if (s_wf.view == VIEW_HONEYPOT && v != VIEW_HONEYPOT && s_wf.hp_wanted) {
        s_wf.hp_wanted = false;
        sentinel_tool("wifi");
    }
    s_wf.view = v;
    for (int i = 0; i < VIEW_N; i++) lv_obj_set_style_text_color(lv_obj_get_child(s_wf.tab[i], 0), i == (int)v ? g_pal.accent : g_pal.dim, 0);
    lv_label_set_text(s_wf.hint, hint[v]);
    lv_obj_clean(s_wf.content);
    memset(s_wf.row, 0, sizeof(s_wf.row));
    s_wf.chart = NULL;

    lv_obj_t *left = deck_box(s_wf.content);
    lv_obj_set_size(left, 440, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *p = rf_panel(left, title[v]);
    lv_obj_set_flex_grow(p, 1);
    lv_obj_t *big = deck_box(p);
    lv_obj_set_width(big, LV_PCT(100));
    lv_obj_set_flex_flow(big, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(big, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(big, 14, 0);
    s_wf.big = deck_label(big, g_font.disp_l, g_pal.text, "0");
    deck_label(big, g_font.mono_m, g_pal.dim, unit[v]);
    for (int i = 0; i < 8; i++) {
        s_wf.line[i] = deck_label(p, i == 5 && v != VIEW_HONEYPOT ? g_font.mono_m : g_font.mono_s, i == 5 ? g_pal.text : g_pal.dim, "");
        lv_obj_set_width(s_wf.line[i], LV_PCT(100));
        lv_label_set_long_mode(s_wf.line[i], LV_LABEL_LONG_MODE_WRAP);
        if (i == 5) lv_obj_set_style_margin_top(s_wf.line[i], 14, 0);
    }

    lv_obj_t *right = deck_box(s_wf.content);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    if (v == VIEW_MONITOR) {
        p = rf_panel(right, "LATEST SWEEP, THEN HISTORY  NEWEST ON TOP   2.4 GHZ  |  5 GHZ");
        lv_obj_set_flex_grow(p, 1);
        s_wf.chart = deck_box(p);
        lv_obj_set_width(s_wf.chart, LV_PCT(100));
        lv_obj_set_flex_grow(s_wf.chart, 1);
        lv_obj_add_event_cb(s_wf.chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);
    } else if (v == VIEW_HONEYPOT) {
        p = rf_panel(right, "WHAT IT IS FOR");
        lv_obj_set_flex_grow(p, 1);
        lv_obj_t *t = deck_label(p, g_font.mono_m, g_pal.dim,
                                 "DEVICES JOIN NETWORKS THEY REMEMBER BY NAME ALONE. AN IMPOSTOR ONLY HAS TO USE THE NAME.\n\n"
                                 "THE HONEYPOT IS THAT IMPOSTOR, HARMLESS: OPEN, NO ADDRESS HANDED OUT, NO TRAFFIC CARRIED. "
                                 "WHAT IT TELLS YOU IS WHICH DEVICES WOULD HAVE FOLLOWED.\n\n"
                                 "IT ALSO GIVES YOUR OWN DETECTORS SOMETHING TO SEE: A NEW OPEN NETWORK, AND WITH A NAME "
                                 "YOU ALREADY USE, A NAME CLASH.\n\n"
                                 "ONLY ON NAMES AND IN PLACES THAT ARE YOURS TO TEST.");
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    } else {
        p = rf_panel(right, v == VIEW_NETWORKS ? "STRONGEST FIRST   dBm  CHANNEL  SECURITY  ADDRESS  NAME" : "dBm  ID  POSITION  ALTITUDE  HEARD ON  LAST HEARD");
        lv_obj_set_flex_grow(p, 1);
        lv_obj_set_style_pad_row(p, 2, 0);
        for (int r = 0; r < ROWS; r++) {
            lv_obj_t *row = deck_label(p, g_font.mono_m, g_pal.dim, "");
            lv_obj_set_width(row, LV_PCT(100));
            lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_bg_color(row, g_pal.panel_hi, 0);
            lv_obj_set_style_pad_ver(row, 4, 0);
            lv_obj_set_style_pad_hor(row, 6, 0);
            lv_obj_set_height(row, lv_font_get_line_height(g_font.mono_m) + 8); /* one line: a long row ends in dots, it does not wrap */
            s_wf.row[r] = row;
        }
    }
    render();
}

void rf_wifi_build(lv_obj_t *parent)
{
    memset(&s_wf, 0, sizeof(s_wf));
    sentinel_wifihost_init(&s_wf.w);
    for (int s = 0; s < SENTINEL_WIFI_CHANNELS; s++) snprintf(s_wf.ch_text[s], sizeof(s_wf.ch_text[s]), "%u", sentinel_wifi_channel[s]);
    s_wf.sel_slot = 5; /* channel 6 */
    if (!hal_cfg_get_str("rf_hp_name", s_wf.hp_ssid, sizeof(s_wf.hp_ssid)) || !s_wf.hp_ssid[0]) snprintf(s_wf.hp_ssid, sizeof(s_wf.hp_ssid), "FreeWiFi");
    s_wf.hp_ch = (uint8_t)hal_cfg_get_i32("rf_hp_ch", 6);
    if (sentinel_wifi_slot(s_wf.hp_ch) < 0) s_wf.hp_ch = 6;
    sentinel_tool("wifi"); /* hop every channel while this screen is up */

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 10, 0);

    lv_obj_t *tabs = deck_box(root);
    lv_obj_set_width(tabs, LV_PCT(100));
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 28, 0);
    for (int i = 0; i < VIEW_N; i++) {
        s_wf.tab[i] = deck_box(tabs);
        deck_label(s_wf.tab[i], g_font.disp_s, g_pal.dim, s_view_name[i]);
        lv_obj_add_flag(s_wf.tab[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(s_wf.tab[i], 10);
        lv_obj_add_event_cb(s_wf.tab[i], tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    s_wf.content = deck_box(root);
    lv_obj_set_width(s_wf.content, LV_PCT(100));
    lv_obj_set_flex_grow(s_wf.content, 1);
    lv_obj_set_flex_flow(s_wf.content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_wf.content, 16, 0);
    s_wf.hint = deck_label(root, g_font.mono_s, g_pal.dim, "");

    sentinel_snapshot(&s_wf.st);
    sentinel_wifi_copy(&s_wf.w);
    set_view(VIEW_MONITOR);
    s_wf.timer = lv_timer_create(tick, TICK_MS, NULL);
}

void rf_wifi_destroy(void)
{
    if (s_wf.timer) lv_timer_delete(s_wf.timer);
    s_wf.timer   = NULL;
    s_wf.content = NULL;
    s_wf.hp_wanted = false;
    sentinel_capture(false); /* capture and the radio (the honeypot included) belong to this screen */
    sentinel_tool("watch");
}

static void name_entered(const char *text, void *user)
{
    (void)user;
    if (!text || !s_wf.timer) return; /* cancelled, or the screen went while the dialog was up */
    size_t n = strlen(text);
    if (n == 0 || n > SENTINEL_WIFI_SSID_MAX) return;
    snprintf(s_wf.hp_ssid, sizeof(s_wf.hp_ssid), "%s", text);
    hal_cfg_set_str("rf_hp_name", s_wf.hp_ssid);
    render();
}

static bool honeypot_key(const deck_key_t *k)
{
    bool on = s_wf.hp_wanted;
    if (k->code == HID_LETTER('N') && !on) {
        deck_modal_prompt("HONEYPOT NAME (1 TO 32 CHARACTERS)", s_wf.hp_ssid, name_entered, NULL);
        return true;
    }
    if (k->code == HID_LETTER('K') && !on) {
        size_t i = 0, count = sizeof(s_hp_channels) / sizeof(s_hp_channels[0]);
        while (i < count && s_hp_channels[i] != s_wf.hp_ch) i++;
        s_wf.hp_ch = s_hp_channels[(i + 1) % count];
        hal_cfg_set_i32("rf_hp_ch", s_wf.hp_ch);
        render();
        return true;
    }
    if (k->code == HID_ENTER) {
        if (rf_mode() != RF_MODE_RED) return true; /* the screen says why */
        s_wf.hp_wanted = !on;
        if (s_wf.hp_wanted) sentinel_honeypot(s_wf.hp_ch, s_wf.hp_ssid);
        else sentinel_tool("wifi");
        render();
        return true;
    }
    return false;
}

bool rf_wifi_key(const deck_key_t *k)
{
    if (!s_wf.timer || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) return false;
    if (k->code >= HID_1 && k->code < HID_1 + VIEW_N) {
        set_view((wifi_view_t)(k->code - HID_1));
        return true;
    }
    if (s_wf.view == VIEW_HONEYPOT) return honeypot_key(k);
    if (k->code == HID_LETTER('C') && s_wf.view != VIEW_DRONES) {
        s_wf.cap_wanted = !s_wf.cap_wanted;
        sentinel_capture(s_wf.cap_wanted);
        render();
        return true;
    }
    if (s_wf.view == VIEW_MONITOR && (k->code == HID_LEFT || k->code == HID_RIGHT)) {
        s_wf.sel_slot = (s_wf.sel_slot + (k->code == HID_RIGHT ? 1 : SENTINEL_WIFI_CHANNELS - 1)) % SENTINEL_WIFI_CHANNELS;
        render();
        return true;
    }
    if (s_wf.view == VIEW_NETWORKS && (k->code == HID_UP || k->code == HID_DOWN)) {
        s_wf.sel_index += k->code == HID_DOWN ? 1 : -1;
        render();
        return true;
    }
    return false;
}
