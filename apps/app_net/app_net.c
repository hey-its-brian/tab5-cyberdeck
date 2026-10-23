/*
 * NET TOOLS: the Tab5's own radio as a toolkit. Four tabs: SCAN (every
 * access point in earshot with a channel chart), PING (ping, DNS,
 * traceroute, port check), LAN (who is on the subnet) and SPEED (down and
 * up). SCAN is built; the other three are on their way.
 */
#include "app_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "net_tools.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TABS 4
#define TAB_SCAN 0
#define TAB_PING 1
#define TAB_LAN 2
#define TAB_SPEED 3

#define BSS_MAX 48
#define CHANNELS 13
#define RESCAN_MS 10000

#define HID_R 0x15

static const char *const s_tab_names[TABS] = {"SCAN", "PING", "LAN", "SPEED"};

static struct {
    int tab;
    lv_obj_t *tab_btn[TABS];
    lv_obj_t *page[TABS];
    lv_obj_t *status;

    /* SCAN */
    net_bss_t bss[BSS_MAX];
    int count;
    int sel; /* index into bss, -1 none */
    lv_obj_t *list;
    lv_obj_t *chart;
    lv_chart_series_t *strength;
    lv_obj_t *ch_count[CHANNELS];
    lv_obj_t *detail;
    bool scanning;
    uint32_t scanned_at; /* lv tick of the last finished scan, 0 = none */
    lv_timer_t *timer;

    /* PING */
    lv_obj_t *host_ta;
    lv_obj_t *out;       /* the results label */
    lv_obj_t *out_box;
    lv_obj_t *ping_btn[4];
    char host[96];
    int job;             /* which tool is running, -1 none */
    int port;
    char result[2048];
    net_ping_reply_t pings[5];
    int ping_got;
    net_hop_t hops[20];
    int hop_n;
    bool port_ok;
    float port_ms;
    char ip[16];
    bool ok;
} s_ui DECK_PSRAM_BSS;

enum { JOB_PING, JOB_DNS, JOB_TRACE, JOB_PORT };

/* ---- Helpers ------------------------------------------------------------- */

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = deck_button(parent, text, NULL);
    lv_obj_set_style_min_height(b, 46, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

static int by_rssi(const void *a, const void *b)
{
    const net_bss_t *x = a, *y = b;
    return y->rssi - x->rssi;
}

static const char *signal_word(int rssi)
{
    return rssi >= -55 ? "EXCELLENT" : rssi >= -65 ? "GOOD" : rssi >= -75 ? "FAIR" : "WEAK";
}

static const char *bars(int rssi)
{
    static const char *const plain[] = {"|", "||", "|||", "||||"}; /* the mono font has no block glyphs */
    return plain[rssi >= -55 ? 3 : rssi >= -65 ? 2 : rssi >= -75 ? 1 : 0];
}

static int channel_mhz(int ch) { return ch == 14 ? 2484 : 2407 + ch * 5; }

/* ---- SCAN ---------------------------------------------------------------- */

static void render_detail(void)
{
    if (s_ui.sel < 0 || s_ui.sel >= s_ui.count) {
        lv_label_set_text(s_ui.detail, "Select a network for its details.");
        return;
    }
    const net_bss_t *b = &s_ui.bss[s_ui.sel];
    lv_label_set_text_fmt(s_ui.detail,
                          "SSID      %s\n"
                          "BSSID     %02X:%02X:%02X:%02X:%02X:%02X\n"
                          "CHANNEL   %d  (%d MHz)\n"
                          "SIGNAL    %d dBm  %s\n"
                          "SECURITY  %s",
                          b->ssid[0] ? b->ssid : "(hidden)", b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3],
                          b->bssid[4], b->bssid[5], b->channel, channel_mhz(b->channel), b->rssi, signal_word(b->rssi),
                          b->auth);
}

static void row_clicked(lv_event_t *e)
{
    s_ui.sel = (int)(intptr_t)lv_event_get_user_data(e);
    render_detail();
}

static void render_scan(void)
{
    lv_obj_clean(s_ui.list);
    lv_group_remove_all_objs(deck_input_group());
    if (s_ui.count == 0) {
        deck_label(s_ui.list, g_font.mono_m, g_pal.dim,
                   net_state() == NET_NO_RADIO ? "No radio." : s_ui.scanning ? "Scanning..." : "Nothing heard yet.");
    }
    for (int i = 0; i < s_ui.count; i++) {
        const net_bss_t *b = &s_ui.bss[i];
        lv_obj_t *row      = deck_panel(s_ui.list, DECK_CUT_BR, 10);
        lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(deck_input_group(), row);
        if (i == s_ui.sel) lv_obj_add_state(row, LV_STATE_CHECKED);

        lv_obj_t *name = deck_label(row, g_font.mono_m, b->ssid[0] ? g_pal.text : g_pal.dim,
                                    b->ssid[0] ? b->ssid : "(hidden)");
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(name, 300);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        char ch[12];
        snprintf(ch, sizeof(ch), "CH %d", b->channel);
        lv_obj_t *c = deck_label(row, g_font.mono_s, g_pal.accent2, ch);
        lv_obj_align(c, LV_ALIGN_LEFT_MID, 320, 0);

        lv_obj_t *auth = deck_label(row, g_font.mono_s, strcmp(b->auth, "OPEN") == 0 ? g_pal.warn : g_pal.dim, b->auth);
        lv_obj_align(auth, LV_ALIGN_LEFT_MID, 400, 0);

        char sig[24];
        snprintf(sig, sizeof(sig), "%d  %s", b->rssi, bars(b->rssi));
        lv_obj_t *s = deck_label(row, g_font.mono_s, g_pal.text, sig);
        lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    /* Channel chart: the strongest signal on each channel, with a count. */
    int best[CHANNELS + 1], count[CHANNELS + 1];
    for (int c = 0; c <= CHANNELS; c++) {
        best[c]  = -100;
        count[c] = 0;
    }
    for (int i = 0; i < s_ui.count; i++) {
        int c = s_ui.bss[i].channel;
        if (c < 1 || c > CHANNELS) continue;
        count[c]++;
        if (s_ui.bss[i].rssi > best[c]) best[c] = s_ui.bss[i].rssi;
    }
    for (int c = 1; c <= CHANNELS; c++) {
        int v = count[c] ? (best[c] + 100) * 100 / 70 : 0; /* -100..-30 dBm to 0..100 */
        if (v > 100) v = 100;
        if (count[c] && v < 6) v = 6; /* a heard network always shows a stub */
        lv_chart_set_series_value_by_id(s_ui.chart, s_ui.strength, (uint32_t)(c - 1), count[c] ? v : LV_CHART_POINT_NONE);
        if (count[c]) {
            lv_label_set_text_fmt(s_ui.ch_count[c - 1], "%d", count[c]);
        } else {
            lv_label_set_text(s_ui.ch_count[c - 1], "");
        }
    }
    lv_chart_refresh(s_ui.chart);
    render_detail();
}

static void render_status(void)
{
    char buf[96];
    if (s_ui.scanning) {
        snprintf(buf, sizeof(buf), "SCANNING...");
    } else if (s_ui.scanned_at == 0) {
        snprintf(buf, sizeof(buf), "NO SCAN YET  ::  R RESCAN");
    } else {
        unsigned age = lv_tick_elaps(s_ui.scanned_at) / 1000;
        snprintf(buf, sizeof(buf), "%d NETWORK%s  ::  SCANNED %u S AGO  ::  R RESCAN", s_ui.count,
                 s_ui.count == 1 ? "" : "S", age);
    }
    lv_label_set_text(s_ui.status, buf);
}

static void start_scan(void)
{
    if (s_ui.scanning) return;
    if (net_scan_start()) s_ui.scanning = true;
    render_status();
}

static void scan_tick(lv_timer_t *t)
{
    (void)t;
    if (s_ui.tab != TAB_SCAN) return;
    if (s_ui.scanning && net_scan_done()) {
        s_ui.scanning = false;
        s_ui.count    = (int)net_scan_bss(s_ui.bss, BSS_MAX);
        qsort(s_ui.bss, (size_t)s_ui.count, sizeof(s_ui.bss[0]), by_rssi);
        s_ui.scanned_at = lv_tick_get();
        if (s_ui.sel >= s_ui.count) s_ui.sel = s_ui.count ? 0 : -1;
        render_scan();
    } else if (!s_ui.scanning && (s_ui.scanned_at == 0 || lv_tick_elaps(s_ui.scanned_at) >= RESCAN_MS)) {
        start_scan();
    }
    render_status();
}

static void build_scan(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(page, 20, 0);

    lv_obj_t *left = deck_panel(page, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_size(left, 640, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left, 16, 0);
    lv_obj_set_style_pad_row(left, 8, 0);
    deck_section(left, "NETWORKS");
    s_ui.list = deck_box(left);
    lv_obj_set_width(s_ui.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.list, 1);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, 8, 0);
    lv_obj_set_style_pad_all(s_ui.list, 4, 0);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_ui.list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *right = deck_panel(page, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right, 16, 0);
    lv_obj_set_style_pad_row(right, 8, 0);
    deck_section(right, "CHANNELS 2.4 GHZ");

    s_ui.chart = lv_chart_create(right);
    lv_obj_set_width(s_ui.chart, LV_PCT(100));
    lv_obj_set_height(s_ui.chart, 220);
    lv_chart_set_type(s_ui.chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(s_ui.chart, CHANNELS);
    lv_chart_set_div_line_count(s_ui.chart, 4, 0);
    lv_chart_set_axis_range(s_ui.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(s_ui.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_ui.chart, 0, 0);
    lv_obj_set_style_pad_all(s_ui.chart, 4, 0);
    lv_obj_set_style_pad_column(s_ui.chart, 6, 0);
    lv_obj_set_style_line_color(s_ui.chart, g_pal.grid, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.chart, 0, LV_PART_ITEMS);
    s_ui.strength = lv_chart_add_series(s_ui.chart, g_pal.accent, LV_CHART_AXIS_PRIMARY_Y);

    /* Channel numbers under the bars, with how many networks sit on each. */
    lv_obj_t *nums = deck_box(right);
    lv_obj_set_width(nums, LV_PCT(100));
    lv_obj_set_flex_flow(nums, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nums, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int c = 1; c <= CHANNELS; c++) {
        lv_obj_t *cell = deck_box(nums);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        char n[4];
        snprintf(n, sizeof(n), "%d", c);
        deck_label(cell, g_font.mono_s, g_pal.dim, n);
        s_ui.ch_count[c - 1] = deck_label(cell, g_font.mono_s, g_pal.accent2, "");
    }
    lv_obj_t *key = deck_label(right, g_font.mono_s, g_pal.dim, "BAR: STRONGEST SIGNAL    NUMBER: NETWORKS");
    lv_obj_set_width(key, LV_PCT(100));

    deck_section(right, "SELECTED");
    s_ui.detail = deck_label(right, g_font.mono_m, g_pal.text, "");
    lv_obj_set_width(s_ui.detail, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_ui.detail, 6, 0);
}

/* ---- PING / DNS / TRACE / PORT ------------------------------------------- */

static void ping_set_busy(bool busy)
{
    for (int i = 0; i < 4; i++) {
        if (busy) {
            lv_obj_add_state(s_ui.ping_btn[i], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(s_ui.ping_btn[i], LV_STATE_DISABLED);
        }
    }
}

/* Worker task: do the blocking work into s_ui, nothing LVGL. */
static void ping_job(void *u)
{
    (void)u;
    switch (s_ui.job) {
        case JOB_PING:
            s_ui.ping_got = net_ping(s_ui.host, 5, 1500, s_ui.pings);
            s_ui.ok       = s_ui.ping_got >= 0;
            break;
        case JOB_DNS: s_ui.ok = net_resolve(s_ui.host, s_ui.ip, sizeof(s_ui.ip)); break;
        case JOB_TRACE:
            s_ui.hop_n = net_trace(s_ui.host, 20, 1200, s_ui.hops);
            s_ui.ok    = s_ui.hop_n >= 0;
            break;
        case JOB_PORT: s_ui.ok = net_port_open(s_ui.host, s_ui.port, 2500, &s_ui.port_ms); break;
        default: break;
    }
}

static void ping_done(void *u)
{
    (void)u;
    if (s_ui.out == NULL) return; /* the module closed meanwhile */
    char *r      = s_ui.result;
    size_t n     = sizeof(s_ui.result);
    size_t o     = 0;
    switch (s_ui.job) {
        case JOB_PING:
            if (!s_ui.ok) {
                o += (size_t)snprintf(r + o, n - o, "PING %s\ncould not resolve or open a socket", s_ui.host);
                break;
            }
            o += (size_t)snprintf(r + o, n - o, "PING %s  (5 x 32 bytes)\n", s_ui.host);
            float min = 1e9f, max = 0, sum = 0;
            for (int i = 0; i < 5 && o < n - 80; i++) {
                const net_ping_reply_t *p = &s_ui.pings[i];
                if (p->ok) {
                    o += (size_t)snprintf(r + o, n - o, "  seq %d  from %-15s  ttl %-3d  %6.1f ms\n", p->seq, p->from, p->ttl,
                                          (double)p->ms);
                    if (p->ms < min) min = p->ms;
                    if (p->ms > max) max = p->ms;
                    sum += p->ms;
                } else {
                    o += (size_t)snprintf(r + o, n - o, "  seq %d  timeout\n", p->seq);
                }
            }
            if (s_ui.ping_got) {
                o += (size_t)snprintf(r + o, n - o, "\n%d of 5 answered  min %.1f  avg %.1f  max %.1f ms", s_ui.ping_got,
                                      (double)min, (double)(sum / (float)s_ui.ping_got), (double)max);
            } else {
                o += (size_t)snprintf(r + o, n - o, "\nno answer: host down, or ICMP blocked on the way");
            }
            break;
        case JOB_DNS:
            o += (size_t)snprintf(r + o, n - o, "DNS %s\n%s", s_ui.host, s_ui.ok ? s_ui.ip : "no such name");
            break;
        case JOB_TRACE:
            if (!s_ui.ok) {
                o += (size_t)snprintf(r + o, n - o, "TRACE %s\ncould not resolve or open a socket", s_ui.host);
                break;
            }
            o += (size_t)snprintf(r + o, n - o, "TRACE %s  (one probe per hop)\n", s_ui.host);
            for (int i = 0; i < s_ui.hop_n && o < n - 60; i++) {
                const net_hop_t *h = &s_ui.hops[i];
                if (h->ip[0]) {
                    o += (size_t)snprintf(r + o, n - o, "  %2d  %-15s  %6.1f ms%s\n", h->hop, h->ip, (double)h->ms,
                                          h->reached ? "  <- destination" : "");
                } else {
                    o += (size_t)snprintf(r + o, n - o, "  %2d  *\n", h->hop);
                }
            }
            if (s_ui.hop_n && !s_ui.hops[s_ui.hop_n - 1].reached) {
                o += (size_t)snprintf(r + o, n - o, "\nstopped without reaching it");
            }
            break;
        case JOB_PORT:
            o += (size_t)snprintf(r + o, n - o, "PORT %s:%d\n%s (%.0f ms)", s_ui.host, s_ui.port,
                                  s_ui.ok ? "OPEN: something is listening" : "CLOSED or filtered", (double)s_ui.port_ms);
            break;
        default: break;
    }
    lv_label_set_text(s_ui.out, s_ui.result);
    s_ui.job = -1;
    ping_set_busy(false);
}

static bool take_host(void)
{
    const char *t = lv_textarea_get_text(s_ui.host_ta);
    while (*t == ' ') t++;
    if (*t == '\0') {
        lv_label_set_text(s_ui.out, "Type a host name or address first.");
        return false;
    }
    snprintf(s_ui.host, sizeof(s_ui.host), "%s", t);
    for (char *p = s_ui.host; *p; p++) {
        if (*p == ' ' || *p == '/') {
            *p = '\0';
            break;
        }
    }
    return true;
}

static void run_job(int job)
{
    if (net_tools_busy() || !take_host()) return;
    if (net_state() != NET_CONNECTED) {
        lv_label_set_text(s_ui.out, "No network. Join a Wi-Fi network in SYSTEM first.");
        return;
    }
    s_ui.job = job;
    static const char *const names[] = {"PINGING", "RESOLVING", "TRACING", "CHECKING PORT"};
    lv_label_set_text_fmt(s_ui.out, "%s %s...", names[job], s_ui.host);
    ping_set_busy(true);
    if (!net_tools_run(ping_job, ping_done, NULL)) {
        lv_label_set_text(s_ui.out, "Could not start the job.");
        s_ui.job = -1;
        ping_set_busy(false);
    }
}

static void port_entered(const char *text, void *u)
{
    (void)u;
    if (text == NULL) return;
    int port = atoi(text);
    if (port < 1 || port > 65535) {
        lv_label_set_text(s_ui.out, "Port must be 1 to 65535.");
        return;
    }
    s_ui.port = port;
    run_job(JOB_PORT);
}

static void ping_btn_clicked(lv_event_t *e)
{
    int job = (int)(intptr_t)lv_event_get_user_data(e);
    if (job == JOB_PORT) {
        if (!take_host()) return;
        char cur[8];
        snprintf(cur, sizeof(cur), "%d", s_ui.port ? s_ui.port : 443);
        deck_modal_prompt("PORT", cur, port_entered, NULL);
        return;
    }
    run_job(job);
}

static void host_enter(lv_event_t *e)
{
    (void)e;
    run_job(JOB_PING);
}

static void build_ping(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 14, 0);

    lv_obj_t *row = deck_box(page);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    s_ui.host_ta = lv_textarea_create(row);
    lv_textarea_set_one_line(s_ui.host_ta, true);
    lv_textarea_set_placeholder_text(s_ui.host_ta, "host name or address");
    lv_textarea_set_max_length(s_ui.host_ta, 90);
    lv_obj_set_width(s_ui.host_ta, 520);
    lv_obj_set_style_text_font(s_ui.host_ta, g_font.mono_m, 0);
    lv_obj_add_event_cb(s_ui.host_ta, host_enter, LV_EVENT_READY, NULL);
    static const char *const labels[] = {"PING", "DNS", "TRACE", "PORT"};
    for (int i = 0; i < 4; i++) s_ui.ping_btn[i] = small_button(row, labels[i], ping_btn_clicked, i);

    s_ui.out_box = deck_panel(page, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_width(s_ui.out_box, LV_PCT(100));
    lv_obj_set_flex_grow(s_ui.out_box, 1);
    lv_obj_set_style_pad_all(s_ui.out_box, 18, 0);
    lv_obj_add_flag(s_ui.out_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_ui.out_box, LV_DIR_VER);
    s_ui.out = deck_label(s_ui.out_box, g_font.mono_m, g_pal.text, "Enter a host, then PING (or Enter), DNS, TRACE or PORT.");
    lv_obj_set_width(s_ui.out, LV_PCT(100));
    lv_obj_set_style_text_line_space(s_ui.out, 6, 0);
}

/* ---- The other tabs, until they land ------------------------------------- */

static void build_soon(lv_obj_t *page, const char *title, const char *body)
{
    lv_obj_t *p = deck_panel(page, DECK_CUT_TL | DECK_CUT_BR, 18);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(p, 20, 0);
    lv_obj_set_style_pad_row(p, 12, 0);
    deck_section(p, title);
    lv_obj_t *t = deck_label(p, g_font.mono_m, g_pal.dim, body);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_style_text_line_space(t, 6, 0);
}

/* ---- Tabs ---------------------------------------------------------------- */

static void set_tab(int tab)
{
    s_ui.tab = tab;
    for (int i = 0; i < TABS; i++) {
        if (i == tab) {
            lv_obj_remove_flag(s_ui.page[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ui.page[i], LV_OBJ_FLAG_HIDDEN);
        }
        deck_panel_set_outline(s_ui.tab_btn[i], i == tab ? g_pal.accent : g_pal.line);
    }
    lv_group_remove_all_objs(deck_input_group());
    if (tab == TAB_SCAN) {
        render_scan();
        render_status();
    } else if (tab == TAB_PING) {
        lv_group_add_obj(deck_input_group(), s_ui.host_ta);
        for (int i = 0; i < 4; i++) lv_group_add_obj(deck_input_group(), s_ui.ping_btn[i]);
        lv_group_focus_obj(s_ui.host_ta);
        lv_label_set_text(s_ui.status, "");
    } else {
        lv_label_set_text(s_ui.status, "");
    }
}

static void tab_clicked(lv_event_t *e) { set_tab((int)(intptr_t)lv_event_get_user_data(e)); }

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.sel = -1;

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 14, 0);

    lv_obj_t *bar = deck_box(root);
    lv_obj_set_width(bar, LV_PCT(100));
    lv_obj_t *tabs = deck_box(bar);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 10, 0);
    for (int i = 0; i < TABS; i++) {
        s_ui.tab_btn[i] = small_button(tabs, s_tab_names[i], tab_clicked, i);
        lv_group_remove_obj(s_ui.tab_btn[i]); /* 1..4 switch tabs */
    }
    s_ui.status = deck_label(bar, g_font.mono_s, g_pal.dim, "");
    lv_obj_align(s_ui.status, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *content = deck_box(root);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    for (int i = 0; i < TABS; i++) {
        s_ui.page[i] = deck_box(content);
        lv_obj_set_size(s_ui.page[i], LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(s_ui.page[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_ui.job = -1;
    build_scan(s_ui.page[TAB_SCAN]);
    build_ping(s_ui.page[TAB_PING]);
    build_soon(s_ui.page[TAB_LAN], "LAN DISCOVERY",
               "Everything on your subnet: mDNS names and services, then a ping sweep for the quiet ones, with vendors from the MAC prefix.\n\nNext build.");
    build_soon(s_ui.page[TAB_SPEED], "SPEED TEST",
               "Download and upload throughput of this link, measured against a public test endpoint.\n\nNext build.");

    deck_label(root, g_font.mono_s, g_pal.dim, "[1..4] TAB    [R] RESCAN    [UP/DOWN] SELECT    [TAB] NEXT FIELD    [ESC] BACK");

    s_ui.timer = lv_timer_create(scan_tick, 1000, NULL);
    set_tab(TAB_SCAN);
    start_scan();
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
    /* On the PING tab the digits and letters belong to the host field. */
    bool typing = s_ui.tab == TAB_PING && lv_group_get_focused(deck_input_group()) == s_ui.host_ta;
    if (!typing && k->code >= HID_1 && k->code < HID_1 + TABS) {
        set_tab(k->code - HID_1);
        return true;
    }
    if (typing) return false;
    if (k->code == HID_R && s_ui.tab == TAB_SCAN) {
        s_ui.scanned_at = 0;
        start_scan();
        return true;
    }
    return false;
}

static deck_app_t s_app = {
    .name     = "NET TOOLS",
    .tagline  = "WI-FI + LAN",
    .icon     = ICON_LAN,
    .eta      = NULL,
    .hotkey   = HID_LETTER('R'),
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_net(void) { return &s_app; }
