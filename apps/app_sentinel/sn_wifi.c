/*
 * SENTINEL > WI-FI: what Wi-Fi is on the air around the deck, one job to a
 * screen.
 *
 *   NETWORKS      every access point in range; a row opens its details
 *   CHANNELS      how busy each channel is, now and lately
 *   BEST CHANNEL  which of 1, 6 and 11 a network of your own should use
 *   DRONES        aircraft broadcasting Remote ID
 *   CAPTURE       record the frames in the air to a pcap file
 *
 * With the radio board linked, these listen with it: it hops all 38
 * channels while one of them is open, hears without sending, and runs the
 * detectors (see sentinel_wifimon.h). Without the board, NETWORKS falls
 * back to the deck's own Wi-Fi, which scans actively and knows less.
 *
 * The capture screen here also serves BLUETOOTH > CAPTURE: the node streams
 * whichever radio its tool has.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_net.h"
#include "deck_theme.h"
#include "sentinel_cmd.h"

#define TICK_MS   500
#define ROWS_MAX  64
#define BARS_H    110
#define LABEL_H   26
#define BAND_GAP  14
#define RESCAN_MS 30000
#define DRONE_ROWS 6

/* One access point, from either source. */
typedef struct {
    char     ssid[33], mac[18], sec[12];
    uint8_t  ch;
    int8_t   rssi;
    bool     open, weak, pmf, twin; /* twin: the same name exists with the opposite kind of security */
    uint32_t beacons, first_ms;
} ap_t;

static struct {
    lv_timer_t *timer;
    void (*render)(void);
    bool      board;   /* the list comes from the radio board, not the deck's own Wi-Fi */
    /* NETWORKS */
    sn_list_t list;
    lv_obj_t *summary;
    ap_t      ap[ROWS_MAX];
    int       n;
    char      sel_mac[18];
    bool      detail, scanning;
    uint32_t  scanned_at;
    /* whatever the page is */
    lv_obj_t *state, *v[8], *chart;
    int       sel_slot;
    char      ch_text[SENTINEL_WIFI_CHANNELS][4];
    /* CAPTURE */
    const char *cap_tool;
    bool        cap_wanted;
    uint32_t    cap_base, lost_base;
    sentinel_state_t    st;
    sentinel_wifihost_t w;
} s_wf DECK_PSRAM_BSS;

static sentinel_bandhost_t s_band DECK_PSRAM_BSS; /* BEST CHANNEL's measured energy */

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_wf.st);
    s_wf.board = sn_board_problem(&s_wf.st) == NULL;
    if (s_wf.board) sentinel_wifi_copy(&s_wf.w);
    if (s_wf.render) s_wf.render();
}

/* Every Wi-Fi screen starts the same way: the node hops every channel while it is up. */
static void begin(void (*render)(void), const char *tool)
{
    memset(&s_wf, 0, sizeof(s_wf));
    sentinel_wifihost_init(&s_wf.w);
    for (int s = 0; s < SENTINEL_WIFI_CHANNELS; s++) snprintf(s_wf.ch_text[s], sizeof(s_wf.ch_text[s]), "%u", sentinel_wifi_channel[s]);
    s_wf.sel_slot = 5; /* channel 6 */
    s_wf.render   = render;
    sentinel_tool(tool);
    sentinel_snapshot(&s_wf.st);
    s_wf.board = sn_board_problem(&s_wf.st) == NULL;
    if (s_wf.board) sentinel_wifi_copy(&s_wf.w);
    s_wf.timer = lv_timer_create(tick, TICK_MS, NULL);
}

static void end(void)
{
    if (s_wf.timer) lv_timer_delete(s_wf.timer);
    s_wf.timer  = NULL;
    s_wf.render = NULL;
    s_wf.chart  = NULL;
    memset(&s_wf.list, 0, sizeof(s_wf.list));
    sentinel_capture(false); /* a capture and the radio belong to the screen that asked */
    sentinel_tool("watch");
}

/* ---- NETWORKS ---------------------------------------------------------------- */

/* A bounded copy the compiler can see the bound of. */
static void copy(char *dst, size_t n, const char *src)
{
    size_t len = strnlen(src, n - 1);
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int by_rssi(const void *a, const void *b) { return ((const ap_t *)b)->rssi - ((const ap_t *)a)->rssi; }

static void aps_from_board(void)
{
    const sentinel_wifihost_t *w = &s_wf.w;
    s_wf.n = w->n > ROWS_MAX ? ROWS_MAX : w->n;
    for (int i = 0; i < s_wf.n; i++) {
        const sentinel_wifitab_ap_t *e = &w->ap[i];
        ap_t                        *a = &s_wf.ap[i];
        memset(a, 0, sizeof(*a));
        copy(a->ssid, sizeof(a->ssid), e->d.ssid);
        sentinel_mac_text(a->mac, e->d.bssid);
        copy(a->sec, sizeof(a->sec), sentinel_sec_name(e->d.sec));
        a->ch = e->d.ch, a->rssi = e->d.rssi, a->pmf = e->d.pmf;
        a->open = e->d.sec == SENTINEL_SEC_OPEN, a->weak = !a->open && e->d.sec <= SENTINEL_SEC_WPA;
        a->beacons = e->d.beacons, a->first_ms = e->first_ms;
    }
}

static void aps_from_deck(void)
{
    static net_bss_t bss[ROWS_MAX];
    if (s_wf.scanning && net_scan_done()) {
        s_wf.scanning   = false;
        s_wf.scanned_at = lv_tick_get();
        s_wf.n          = (int)net_scan_bss(bss, ROWS_MAX);
        for (int i = 0; i < s_wf.n; i++) {
            ap_t *a = &s_wf.ap[i];
            memset(a, 0, sizeof(*a));
            copy(a->ssid, sizeof(a->ssid), bss[i].ssid);
            snprintf(a->mac, sizeof(a->mac), "%02X:%02X:%02X:%02X:%02X:%02X", bss[i].bssid[0], bss[i].bssid[1], bss[i].bssid[2], bss[i].bssid[3],
                     bss[i].bssid[4], bss[i].bssid[5]);
            copy(a->sec, sizeof(a->sec), bss[i].auth);
            a->ch = bss[i].channel, a->rssi = bss[i].rssi;
            a->open = !strcmp(bss[i].auth, "OPEN"), a->weak = !strcmp(bss[i].auth, "WEP") || !strcmp(bss[i].auth, "WPA");
        }
        qsort(s_wf.ap, (size_t)s_wf.n, sizeof(s_wf.ap[0]), by_rssi);
    } else if (!s_wf.scanning && (s_wf.scanned_at == 0 || lv_tick_elaps(s_wf.scanned_at) >= RESCAN_MS)) {
        s_wf.scanning = net_scan_start();
    }
}

static void aps_pull(void)
{
    if (s_wf.board) aps_from_board();
    else aps_from_deck();
    for (int i = 0; i < s_wf.n; i++) {
        ap_t *a = &s_wf.ap[i];
        a->twin = false;
        if (!a->ssid[0]) continue;
        for (int k = 0; k < s_wf.n; k++)
            if (k != i && !strcmp(s_wf.ap[k].ssid, a->ssid) && s_wf.ap[k].open != a->open) a->twin = true;
    }
}

/* An open copy of a protected name is the likely impostor; anything open or old is weak. */
static sn_level_t ap_level(const ap_t *a) { return a->twin && a->open ? SN_ALERT : (a->twin || a->open || a->weak) ? SN_WARNING : SN_NOTE; }

static int ap_selected(void)
{
    for (int i = 0; i < s_wf.n; i++)
        if (!strcmp(s_wf.ap[i].mac, s_wf.sel_mac)) return i;
    return -1;
}

static void net_list_build(void);

static void net_detail_render(void)
{
    int i = ap_selected();
    if (i < 0) {
        sn_set(s_wf.state, g_pal.label, "NO LONGER HEARD");
        for (int k = 0; k < 5; k++) sn_set(s_wf.v[k], g_pal.label, " ");
        return;
    }
    const ap_t *a = &s_wf.ap[i];
    sn_set(s_wf.state, lv_color_white(), "%s", a->ssid[0] ? a->ssid : "(HIDDEN NAME)");
    if (a->twin && a->open) sn_set(s_wf.v[0], g_pal.danger, "!! This name is protected on another access point and open here. This one may be an impostor.");
    else if (a->twin) sn_set(s_wf.v[0], g_pal.warn, "! An open copy of this name is nearby. Devices may join that one instead.");
    else if (a->open) sn_set(s_wf.v[0], g_pal.warn, "! Open: anyone nearby can read what is not encrypted some other way.");
    else if (a->weak) sn_set(s_wf.v[0], g_pal.warn, "! %s is old and can be broken.", a->sec);
    else sn_set(s_wf.v[0], g_pal.ok, "Nothing unusual about it.");
    sn_set(s_wf.v[1], g_pal.text, "SIGNAL    %d dBm (%s)", a->rssi, a->rssi >= -55 ? "STRONG" : a->rssi >= -70 ? "GOOD" : a->rssi >= -80 ? "WEAK" : "VERY WEAK");
    sn_set(s_wf.v[2], g_pal.text, "CHANNEL   %u (%u MHZ)", a->ch, a->ch <= 14 ? 2407u + 5u * a->ch : 5000u + 5u * a->ch);
    sn_set(s_wf.v[3], g_pal.text, "SECURITY  %s%s", a->sec, a->pmf ? ", MANAGEMENT FRAMES PROTECTED" : "");
    sn_set(s_wf.v[4], g_pal.text, "ADDRESS   %s", a->mac);
    if (s_wf.board) {
        char span[24];
        sn_fmt_span(span, sizeof(span), sentinel_now_ms() - a->first_ms);
        sn_set(s_wf.v[5], g_pal.label, "Heard for %s, %u beacons.", span, (unsigned)a->beacons);
    } else {
        sn_set(s_wf.v[5], g_pal.label, "From the deck's own Wi-Fi scan.");
    }
}

static void net_list_render(void)
{
    int sel = ap_selected();
    if (sel < 0) sel = s_wf.list.sel;
    sn_list_set(&s_wf.list, s_wf.n, sel, s_wf.board ? "LISTENING..." : s_wf.scanning ? "SCANNING..." : "NO NETWORKS FOUND");
    if (s_wf.n) snprintf(s_wf.sel_mac, sizeof(s_wf.sel_mac), "%s", s_wf.ap[s_wf.list.sel].mac);
    int open = 0, weak = 0, twins = 0;
    for (int i = 0; i < s_wf.n; i++) {
        const ap_t *a = &s_wf.ap[i];
        if (a->open) open++;
        if (a->weak) weak++;
        if (a->twin && a->open) twins++;
    }
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        int k = sn_list_item(&s_wf.list, r);
        if (k < 0) continue;
        const ap_t *a   = &s_wf.ap[k];
        sn_level_t  lvl = ap_level(a);
        sn_set(sn_list_text(&s_wf.list, r), lvl == SN_NOTE ? g_pal.text : sn_level_color(lvl), "%-2s %4d dBm   CH %-3u  %-6s  %s", sn_level_mark(lvl), a->rssi, a->ch, a->sec,
               a->ssid[0] ? a->ssid : "(hidden)");
    }
    if (twins) sn_set(s_wf.summary, g_pal.danger, "!! %d POSSIBLE IMPOSTOR%s.   %d OPEN, %d WITH OLD SECURITY, OF %d.", twins, twins == 1 ? "" : "S", open, weak, s_wf.n);
    else
        sn_set(s_wf.summary, g_pal.label, "%d NETWORKS, STRONGEST FIRST.   %d OPEN, %d WITH OLD SECURITY.   %s", s_wf.n, open, weak,
               s_wf.board ? "HEARD BY THE RADIO BOARD." : "SCANNED BY THE DECK'S OWN WI-FI.");
}

static void net_render(void)
{
    aps_pull();
    if (s_wf.detail) net_detail_render();
    else net_list_render();
}

static void net_open(int index)
{
    if (index < 0 || index >= s_wf.n) return;
    snprintf(s_wf.sel_mac, sizeof(s_wf.sel_mac), "%s", s_wf.ap[index].mac);
    s_wf.detail    = true;
    lv_obj_t *body = sn_body_reset();
    memset(&s_wf.list, 0, sizeof(s_wf.list));
    s_wf.state = sn_state(body);
    s_wf.v[0]  = sn_value(body);
    sn_heading(body, "DETAILS");
    for (int i = 1; i < 5; i++) s_wf.v[i] = sn_value(body);
    s_wf.v[5] = sn_note(body);
    net_detail_render();
}

static void net_rescan(void)
{
    if (!s_wf.board) s_wf.scanned_at = 0;
}

static void net_list_build(void)
{
    lv_obj_t *body = sn_body_reset();
    s_wf.detail    = false;
    s_wf.state     = NULL;
    s_wf.summary   = sn_note(body);
    lv_label_set_long_mode(s_wf.summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_wf.summary, lv_font_get_line_height(g_font.mono_m));
    sn_list_build(&s_wf.list, body, net_open);
    if (!s_wf.board) sn_action(HID_LETTER('R'), "R", "SCAN AGAIN", net_rescan);
    sn_help("Every access point in range, strongest first. Press Enter or tap a row for its details.\n\n"
            "!  means weak: open, or WEP or WPA, which can be broken.\n"
            "!! means the same name is protected on one access point and open on another. The open one may be an impostor "
            "set up to catch devices that remember the name.\n\n"
            "With the radio board on, this list is made by listening only. Without it, the deck's own Wi-Fi scans, which "
            "sends probe requests and cannot see as much.");
    net_list_render();
}

static void net_build(lv_obj_t *body)
{
    (void)body;
    begin(net_render, "wifi");
    aps_pull();
    net_list_build();
}

static bool net_back(void)
{
    if (!s_wf.detail) return false;
    net_list_build();
    return true;
}

static bool net_key(const deck_key_t *k)
{
    if (s_wf.detail) return false;
    if (!sn_list_key(&s_wf.list, k)) return false;
    if (!s_wf.detail && s_wf.n) {
        snprintf(s_wf.sel_mac, sizeof(s_wf.sel_mac), "%s", s_wf.ap[s_wf.list.sel].mac);
        net_list_render();
    }
    return true;
}

/* ---- CHANNELS ---------------------------------------------------------------- */

static lv_color_t heat(uint8_t v)
{
    if (v == SENTINEL_WATERFALL_ALARM) return g_pal.danger;
    if (v == 0) return g_pal.bg;
    if (v < 210) return lv_color_mix(g_pal.accent, g_pal.bg, (uint8_t)(12 + v * 243 / 210));
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

static void chart_text(lv_layer_t *layer, int32_t x, int32_t y, const char *s, lv_color_t c)
{
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.text       = s;
    t.text_local = true;
    t.font       = g_font.mono_m;
    t.color      = c;
    lv_area_t a  = {x, y, x + 120, y + LABEL_H};
    lv_draw_label(layer, &t, &a);
}

static int32_t slot_x(const lv_area_t *a, int32_t cw, int s) { return a->x1 + s * cw + (s >= 13 ? BAND_GAP : 0); }

static void chan_draw(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t cw = (w - BAND_GAP) / SENTINEL_WIFI_CHANNELS;
    int32_t top = a.y1 + LABEL_H + 2, fall_y = top + BARS_H + LABEL_H + 6;
    int32_t rh = (a.y2 - fall_y) / SENTINEL_WATERFALL_ROWS;
    if (cw < 3 || rh < 2 || h < BARS_H + 2 * LABEL_H + 40) return;

    for (int s = 0; s < SENTINEL_WIFI_CHANNELS; s++) {
        int32_t               x = slot_x(&a, cw, s);
        const sentinel_wch_t *c = &s_wf.w.ch[s];
        uint8_t v  = c->ms ? sentinel_wifi_intensity(c) : 0;
        int32_t bh = v == SENTINEL_WATERFALL_ALARM ? BARS_H : (int32_t)v * BARS_H / 254;
        if (bh > 0) fill(layer, x, top + BARS_H - bh, x + cw - 2, top + BARS_H, heat(v));
        uint8_t ch = sentinel_wifi_channel[s];
        if ((ch == 1 || ch == 6 || ch == 11 || ch == 36 || ch == 100 || ch == 149) && (s - s_wf.sel_slot > 3 || s_wf.sel_slot - s > 3))
            chart_text(layer, x, top + BARS_H + 3, s_wf.ch_text[s], g_pal.label);
        for (int r = 0; r < SENTINEL_WATERFALL_ROWS; r++) {
            const uint8_t *row = sentinel_wifihost_row(&s_wf.w, r);
            if (!row) break;
            if (row[s]) fill(layer, x, fall_y + r * rh, x + cw - 2, fall_y + r * rh + rh - 2, heat(row[s]));
        }
    }
    fill(layer, a.x1, top + BARS_H, a.x2, top + BARS_H, g_pal.line);
    chart_text(layer, a.x1, a.y1, "2.4 GHZ", g_pal.label);
    chart_text(layer, slot_x(&a, cw, 13), a.y1, "5 GHZ", g_pal.label);
    /* the cursor */
    int32_t cx = slot_x(&a, cw, s_wf.sel_slot) + (cw - 2) / 2;
    fill(layer, cx, top, cx + 1, a.y2, lv_color_white());
    char f[16];
    snprintf(f, sizeof(f), "CH %u", sentinel_wifi_channel[s_wf.sel_slot]);
    int32_t lx = cx - 30;
    if (lx > a.x2 - 80) lx = a.x2 - 80;
    chart_text(layer, lx, top + BARS_H + 3, f, lv_color_white());
}

static void chan_render(void)
{
    const sentinel_wifihost_t *w = &s_wf.w;
    int      busiest = -1, deauth_slot = -1;
    uint32_t frames = 0, ms = 0;
    for (int s = 0; s < SENTINEL_WIFI_CHANNELS; s++) {
        const sentinel_wch_t *c = &w->ch[s];
        if (!c->ms) continue;
        frames += (uint32_t)c->mgmt + c->data + c->ctrl;
        ms += c->ms;
        if (c->deauths) deauth_slot = s;
        if (busiest < 0 || sentinel_wifi_intensity(c) > sentinel_wifi_intensity(&w->ch[busiest])) busiest = s;
    }
    if (!s_wf.st.mon_ok) sn_set(s_wf.state, g_pal.warn, "NEEDS BOARD FIRMWARE 0.5.0");
    else if (deauth_slot >= 0) sn_set(s_wf.state, g_pal.danger, "!! DEAUTH ON CH %u", sentinel_wifi_channel[deauth_slot]);
    else if (busiest >= 0) sn_set(s_wf.state, g_pal.text, "BUSIEST: CH %u", sentinel_wifi_channel[busiest]);
    else sn_set(s_wf.state, g_pal.label, "LISTENING...");
    sn_set(s_wf.v[0], g_pal.text, "%u FRAMES A SECOND HEARD", ms ? (unsigned)(frames * 1000u / ms) : 0u);
    if (deauth_slot >= 0) sn_set(s_wf.v[1], g_pal.danger, "Deauthentication frames knock devices off a network. A burst of them is an attack or a faulty access point.");
    else sn_set(s_wf.v[1], g_pal.label, "Red in the chart means deauthentication frames were heard there.");

    const sentinel_wch_t *c  = &w->ch[s_wf.sel_slot];
    uint8_t               ch = sentinel_wifi_channel[s_wf.sel_slot];
    int8_t                best;
    int                   aps = sentinel_wifihost_aps_on(w, ch, &best);
    sn_set(s_wf.v[2], g_pal.text, "CHANNEL %u, %u MHZ", ch, ch <= 13 ? 2407u + 5u * ch : 5000u + 5u * ch);
    if (aps) sn_set(s_wf.v[3], g_pal.text, "%d NETWORK%s, STRONGEST %d dBm", aps, aps == 1 ? "" : "S", best);
    else sn_set(s_wf.v[3], g_pal.text, "NO NETWORKS HEARD HERE");
    if (c->ms) sn_set(s_wf.v[4], c->deauths ? g_pal.danger : g_pal.label, "Last visit: %u beacons, %u probes, %u data, %u deauth.", c->beacons, c->probes, c->data, c->deauths);
    else sn_set(s_wf.v[4], g_pal.label, "Not visited yet.");
    if (s_wf.chart) lv_obj_invalidate(s_wf.chart);
}

static void chan_move(int d)
{
    s_wf.sel_slot = (s_wf.sel_slot + d + SENTINEL_WIFI_CHANNELS) % SENTINEL_WIFI_CHANNELS;
    chan_render();
}
static void chan_left(void) { chan_move(-1); }
static void chan_right(void) { chan_move(1); }

static void chan_tapped(lv_event_t *e)
{
    lv_obj_t   *obj = lv_event_get_target_obj(e);
    lv_indev_t *in  = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t cw = (lv_area_get_width(&a) - BAND_GAP) / SENTINEL_WIFI_CHANNELS;
    if (cw < 1) return;
    int32_t x = p.x - a.x1;
    if (x > 13 * cw) x -= BAND_GAP;
    int s = (int)(x / cw);
    s_wf.sel_slot = s < 0 ? 0 : s >= SENTINEL_WIFI_CHANNELS ? SENTINEL_WIFI_CHANNELS - 1 : s;
    chan_render();
}

static void chan_build(lv_obj_t *body)
{
    begin(chan_render, "wifi");
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 20, 0);
    lv_obj_t *left = sn_col(body, 440);
    s_wf.state     = sn_state(left);
    s_wf.v[0]      = sn_value(left);
    s_wf.v[1]      = sn_note(left);
    sn_heading(left, "AT THE CURSOR");
    s_wf.v[2] = sn_value(left);
    s_wf.v[3] = sn_value(left);
    s_wf.v[4] = sn_note(left);
    lv_obj_t *right = sn_col(body, 0);
    s_wf.chart      = lv_obj_create(right);
    lv_obj_remove_style_all(s_wf.chart);
    lv_obj_set_width(s_wf.chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_wf.chart, 1);
    lv_obj_add_flag(s_wf.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wf.chart, chan_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_wf.chart, chan_tapped, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_wf.chart, chan_tapped, LV_EVENT_PRESSING, NULL);
    sn_action(HID_LEFT, "<", "CHANNEL DOWN", chan_left);
    sn_action(HID_RIGHT, ">", "CHANNEL UP", chan_right);
    sn_help("One column for each of the 38 Wi-Fi channels the board visits: 2.4 GHz on the left, 5 GHz on the right. The "
            "bars are the latest visit; under them is the history, newest on top.\n\n"
            "Brighter means more frames were heard. Red means deauthentication frames were heard on that channel.\n\n"
            "The board listens and sends nothing.");
    chan_render();
}

/* ---- BEST CHANNEL -------------------------------------------------------------- */

static void best_render(void)
{
    static const uint8_t pick[3] = {1, 6, 11};
    sentinel_band_copy(&s_band);
    const sentinel_wifihost_t *w = &s_wf.w;
    int   counts[3], by_count = 0, by_load = 0;
    float load[3];
    bool  measured = s_band.nrf_stat[0].n > 0;
    for (int i = 0; i < 3; i++) {
        counts[i] = 0;
        for (int k = 0; k < w->n; k++) {
            int d = (int)w->ap[k].d.ch - pick[i];
            if (w->ap[k].d.ch <= 13 && d >= -2 && d <= 2) counts[i]++; /* a neighbour two channels over still overlaps */
        }
        load[i] = sentinel_bandhost_wifi_load(&s_band, pick[i]);
        if (counts[i] < counts[by_count]) by_count = i;
        if (load[i] < load[by_load]) by_load = i;
    }
    int best = measured ? by_load : by_count;
    if (!w->n && !measured) sn_set(s_wf.state, g_pal.label, "STILL LISTENING...");
    else sn_set(s_wf.state, g_pal.ok, "USE CHANNEL %u", pick[best]);
    if (measured && by_load != by_count)
        sn_set(s_wf.v[0], g_pal.text, "It carries the least radio energy. Channel %u has fewer networks, but more is being sent on it.", pick[by_count]);
    else if (measured) sn_set(s_wf.v[0], g_pal.text, "It carries the least radio energy and has the fewest networks on it.");
    else sn_set(s_wf.v[0], g_pal.text, "It has the fewest networks on it. No energy measurement yet.");
    for (int i = 0; i < 3; i++) {
        if (measured)
            sn_set(s_wf.v[1 + i], i == best ? g_pal.ok : g_pal.text, "%s CHANNEL %-2u   %2d NETWORK%s OVERLAP   BUSY %2.0f %% OF THE TIME", i == best ? ">" : " ", pick[i], counts[i],
                   counts[i] == 1 ? " " : "S", (double)load[i] * 100.0 / 8.0);
        else
            sn_set(s_wf.v[1 + i], i == best ? g_pal.ok : g_pal.text, "%s CHANNEL %-2u   %2d NETWORK%s OVERLAP", i == best ? ">" : " ", pick[i], counts[i], counts[i] == 1 ? " " : "S");
    }
}

static void best_build(lv_obj_t *body)
{
    begin(best_render, "wifi");
    s_wf.state = sn_state(body);
    s_wf.v[0]  = sn_value(body);
    sn_heading(body, "THE THREE CHANNELS THAT DO NOT OVERLAP EACH OTHER");
    for (int i = 1; i < 4; i++) s_wf.v[i] = sn_value(body);
    sn_help("On 2.4 GHz only channels 1, 6 and 11 are far enough apart not to interfere with each other, so a network "
            "should use one of them.\n\n"
            "NETWORKS OVERLAP counts the access points heard on that channel or within two channels of it.\n\n"
            "BUSY is measured by the 2.4 GHz radio: how much of the time something was transmitting there, whoever it "
            "was. A channel with few networks can still be busy.");
    best_render();
}

/* ---- DRONES -------------------------------------------------------------------- */

static void drone_render(void)
{
    int      n   = 0;
    uint32_t now = sentinel_now_ms();
    for (int i = 0; i < SENTINEL_DRONES_MAX && n < DRONE_ROWS; i++) {
        const sentinel_drone_t *d = &s_wf.w.drone[i];
        if (!d->used) continue;
        char mac[18], span[24];
        sentinel_mac_text(mac, d->d.mac);
        sn_fmt_span(span, sizeof(span), now - d->last_ms);
        if (d->d.lat != 0 || d->d.lon != 0)
            sn_set(s_wf.v[1 + n], g_pal.text, "%-20s %.5f, %.5f  %d M UP  %d dBm  %s AGO", d->d.id[0] ? d->d.id : mac, d->d.lat, d->d.lon, (int)d->d.alt_m, d->d.rssi, span);
        else sn_set(s_wf.v[1 + n], g_pal.text, "%-20s NO POSITION GIVEN  %d dBm  %s AGO", d->d.id[0] ? d->d.id : mac, d->d.rssi, span);
        n++;
    }
    for (int r = n; r < DRONE_ROWS; r++) sn_set(s_wf.v[1 + r], g_pal.label, " ");
    if (n) sn_set(s_wf.state, g_pal.warn, "! %d AIRCRAFT ANNOUNCING %s", n, n == 1 ? "ITSELF" : "THEMSELVES");
    else sn_set(s_wf.state, g_pal.ok, "NONE HEARD");
    sn_set(s_wf.v[0], g_pal.label, "%s", n ? "ID, where it says it is, height, signal, last heard." : "An aircraft that broadcasts nothing is not seen.");
}

static void drone_build(lv_obj_t *body)
{
    begin(drone_render, "wifi");
    s_wf.state = sn_state(body);
    s_wf.v[0]  = sn_note(body);
    for (int i = 0; i < DRONE_ROWS; i++) s_wf.v[1 + i] = sn_value(body);
    sn_help("Most drones sold today must broadcast Remote ID: who they are and where, on Wi-Fi or Bluetooth. This lists "
            "what the board has heard.\n\n"
            "The ID and the position are what the aircraft claims. A drone that broadcasts nothing, or was built without "
            "Remote ID, does not appear here.");
    drone_render();
}

/* ---- CAPTURE (Wi-Fi and Bluetooth) ---------------------------------------------- */

static void cap_toggle(void)
{
    s_wf.cap_wanted = !s_wf.cap_wanted;
    sentinel_capture(s_wf.cap_wanted);
    if (s_wf.render) s_wf.render();
}

static void cap_render(void)
{
    const sentinel_state_t *st = &s_wf.st;
    sentinel_store_stats_t  log;
    sentinel_stats(&log);
    if (st->capture) {
        unsigned lost = (unsigned)(log.packets_dropped - s_wf.lost_base);
        sn_set(s_wf.state, g_pal.accent, "RECORDING");
        sn_set(s_wf.v[0], g_pal.text, "%u FRAMES SAVED", (unsigned)(log.packets - s_wf.cap_base));
        if (lost) sn_set(s_wf.v[1], g_pal.warn, "! %u frames did not reach the card.", lost);
        else sn_set(s_wf.v[1], g_pal.label, "Nothing lost on the way to the card.");
    } else {
        s_wf.cap_base  = log.packets;
        s_wf.lost_base = log.packets_dropped;
        if (s_wf.cap_wanted && st->host_baud < SENTINEL_CAP_MIN_BAUD) {
            sn_set(s_wf.state, g_pal.warn, "! CANNOT RECORD");
            sn_set(s_wf.v[0], g_pal.text, "Recording needs the fast link to the board, and it is at %u baud.", (unsigned)st->host_baud);
        } else if (s_wf.cap_wanted) {
            sn_set(s_wf.state, g_pal.label, "STARTING...");
            sn_set(s_wf.v[0], g_pal.text, "Asking the radio board.");
        } else {
            sn_set(s_wf.state, g_pal.text, "NOT RECORDING");
            sn_set(s_wf.v[0], g_pal.text, "Recording stops when you leave this screen.");
        }
        sn_set(s_wf.v[1], g_pal.label, "The file goes in sentinel/ on the SD card as a pcap, which Wireshark opens.");
    }
    sn_action_label(cap_toggle, s_wf.cap_wanted ? "STOP RECORDING" : "START RECORDING");
}

static void cap_build_for(lv_obj_t *body, const char *tool)
{
    begin(cap_render, tool);
    s_wf.cap_tool = tool;
    s_wf.state    = sn_state(body);
    s_wf.v[0]     = sn_value(body);
    s_wf.v[1]     = sn_note(body);
    sn_action(HID_ENTER, "ENTER", "START RECORDING", cap_toggle);
    cap_render();
}

static void cap_wifi_build(lv_obj_t *body)
{
    cap_build_for(body, "wifi");
    sn_help("Saves every Wi-Fi frame the board hears, on whichever channel it is visiting, to a pcap file. Nothing is sent "
            "and nothing is decrypted: protected traffic stays unreadable.\n\n"
            "Capturing other people's traffic may be restricted where you are. Use it on networks that are yours.");
}

static void cap_ble_build(lv_obj_t *body)
{
    cap_build_for(body, "ble");
    sn_help("Saves every Bluetooth advertisement the board hears to a pcap file. Advertisements are what devices broadcast "
            "to anyone; connections between devices are not captured.");
}

/* ---- The menu -------------------------------------------------------------------- */

static void menu_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)level;
    if (sn_board_problem(st)) snprintf(line, n, "The deck's own Wi-Fi can still list networks.");
    else snprintf(line, n, "%u NETWORKS HEARD", (unsigned)st->wifi_total);
}

static const sn_screen_t s_net   = {.name = "NETWORKS", .purpose = "The networks in range. Open one to see if it can be trusted.", .icon = ICON_WIFI, .build = net_build, .destroy = end, .key = net_key, .back = net_back};
static const sn_screen_t s_chan  = {.name = "CHANNELS", .purpose = "How busy each Wi-Fi channel is, now and lately.", .icon = ICON_GAUGE, .flags = SN_BOARD, .build = chan_build, .destroy = end};
static const sn_screen_t s_best  = {.name = "BEST CHANNEL", .purpose = "Which of channels 1, 6 and 11 your own network should use.", .icon = ICON_TOOLS, .flags = SN_BOARD, .build = best_build, .destroy = end};
static const sn_screen_t s_drone = {.name = "DRONES", .purpose = "Aircraft announcing themselves nearby (Remote ID).", .icon = ICON_RADAR, .flags = SN_BOARD, .build = drone_build, .destroy = end};
static const sn_screen_t s_cap   = {.name = "CAPTURE", .purpose = "Record the Wi-Fi frames in the air to a file.", .icon = ICON_SD, .flags = SN_BOARD, .build = cap_wifi_build, .destroy = end};

const sn_screen_t sn_ble_capture_screen = {.name = "CAPTURE", .purpose = "Record Bluetooth advertising to a file.", .icon = ICON_SD, .flags = SN_BOARD, .build = cap_ble_build, .destroy = end};

static const sn_screen_t *const s_kids[] = {&s_net, &s_chan, &s_best, &s_drone, &s_cap};

const sn_screen_t sn_wifi_menu = {
    .name    = "WI-FI",
    .purpose = "The Wi-Fi around you: networks, channels, drones.",
    .icon    = ICON_WIFI,
    .kids    = s_kids,
    .nkids   = 5,
    .status  = menu_status,
};
