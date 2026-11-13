/*
 * SENTINEL > NETWORK: questions about the network the deck has joined,
 * asked with the deck's own Wi-Fi. None of these need the radio board.
 *
 *   PING         does a host answer, and how fast
 *   DNS          what address a name has
 *   TRACEROUTE   the path packets take to a host
 *   PORT CHECK   whether a port is open on a host
 *   LAN DEVICES  who else is on this network; a row opens that device
 *   SPEED TEST   how fast the connection is
 *
 * The work is blocking, so it runs on the net tools worker (net_tools.h)
 * and its results are kept here, outside any screen: a job that finishes
 * after its screen has closed changes nothing on screen.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_net.h"
#include "deck_theme.h"
#include "net_tools.h"

enum { JOB_PING, JOB_DNS, JOB_TRACE, JOB_PORT, JOB_N };

#define SPEED_HOST "speed.cloudflare.com"
/* Plain HTTP for the download so the number is the link, not TLS decryption. */
#define SPEED_DOWN_URL   "http://ipv4.download.thinkbroadband.com/10MB.zip"
#define SPEED_UP_URL     "https://speed.cloudflare.com/__up"
#define SPEED_DOWN_BYTES (8u * 1000u * 1000u)
#define SPEED_UP_BYTES   (2u * 1000u * 1000u)
#define HOSTS_MAX 64

static struct {
    /* what is on screen (NULL when its screen is not up) */
    lv_obj_t   *host_ta, *port_ta, *state, *out, *v[4];
    lv_timer_t *timer;
    int         screen_job; /* which of the four host screens is up, -1 for none */
    sn_list_t   list;
    bool        lan_up, lan_detail, speed_up;
    int         lan_sel;
    /* a host job */
    char host[96];
    int  job, port; /* job: what is running, -1 for nothing */
    bool ok, port_ok;
    char ip[16], result[2048];
    net_ping_reply_t pings[5];
    int   ping_got, hop_n;
    net_hop_t hops[20];
    float port_ms;
    /* LAN */
    net_host_t   hosts[HOSTS_MAX];
    int          host_n;
    volatile int lan_done, lan_total;
    bool         lan_running, lan_ran;
    /* SPEED */
    volatile int spd_phase; /* 0 idle, 1 latency, 2 down, 3 up, 4 done */
    bool   spd_lat_ok, spd_down_ok, spd_up_ok, spd_ran;
    float  spd_lat_ms, spd_down_ms, spd_up_ms;
    size_t spd_down_bytes;
} s_nt DECK_PSRAM_BSS;

static bool s_nt_init;

static void once(void)
{
    if (s_nt_init) return;
    s_nt_init       = true;
    s_nt.job        = -1;
    s_nt.screen_job = -1;
    s_nt.port       = 443;
}

static const char *offline(void) { return net_state() == NET_CONNECTED ? NULL : "NOT JOINED TO A NETWORK"; }

/* ---- PING, DNS, TRACEROUTE, PORT CHECK ---------------------------------------- */

static const char *const s_verb[JOB_N]    = {"PING IT", "LOOK IT UP", "TRACE THE PATH", "CHECK THE PORT"};
static const char *const s_running[JOB_N] = {"PINGING", "LOOKING UP", "TRACING THE PATH TO", "CHECKING"};

static void host_job(void *u)
{
    (void)u;
    switch (s_nt.job) {
        case JOB_PING:
            s_nt.ping_got = net_ping(s_nt.host, 5, 1500, s_nt.pings);
            s_nt.ok       = s_nt.ping_got >= 0;
            break;
        case JOB_DNS: s_nt.ok = net_resolve(s_nt.host, s_nt.ip, sizeof(s_nt.ip)); break;
        case JOB_TRACE:
            s_nt.hop_n = net_trace(s_nt.host, 20, 1200, s_nt.hops);
            s_nt.ok    = s_nt.hop_n >= 0;
            break;
        case JOB_PORT: s_nt.ok = net_port_open(s_nt.host, s_nt.port, 2500, &s_nt.port_ms); break;
        default: break;
    }
}

static void host_run(void);

/* The answer in a few big words, and the detail under it. */
static void host_done(void *u)
{
    (void)u;
    int    job = s_nt.job;
    s_nt.job   = -1;
    if (s_nt.out) sn_action_enable(host_run, true); /* whichever host screen is up can run again */
    if (!s_nt.out || s_nt.screen_job != job) return; /* its own screen has gone */
    char  *r = s_nt.result;
    size_t n = sizeof(s_nt.result), o = 0;
    r[0]     = 0;
    switch (job) {
        case JOB_PING: {
            if (!s_nt.ok) {
                sn_set(s_nt.state, g_pal.warn, "! COULD NOT FIND IT");
                snprintf(r, n, "The name did not resolve, or a socket could not be opened.");
                break;
            }
            float min = 1e9f, max = 0, sum = 0;
            for (int i = 0; i < 5 && o < n - 80; i++) {
                const net_ping_reply_t *p = &s_nt.pings[i];
                if (p->ok) {
                    o += (size_t)snprintf(r + o, n - o, "%d   from %-15s  %6.1f ms\n", p->seq, p->from, (double)p->ms);
                    if (p->ms < min) min = p->ms;
                    if (p->ms > max) max = p->ms;
                    sum += p->ms;
                } else {
                    o += (size_t)snprintf(r + o, n - o, "%d   no answer\n", p->seq);
                }
            }
            if (s_nt.ping_got) sn_set(s_nt.state, g_pal.ok, "%d OF 5 ANSWERED, %.0f MS", s_nt.ping_got, (double)(sum / (float)s_nt.ping_got));
            else sn_set(s_nt.state, g_pal.warn, "! NO ANSWER");
            if (s_nt.ping_got) snprintf(r + o, n - o, "\nFastest %.1f ms, slowest %.1f ms.", (double)min, (double)max);
            else snprintf(r + o, n - o, "\nThe host is down, or something on the way blocks ping.");
            break;
        }
        case JOB_DNS:
            if (s_nt.ok) sn_set(s_nt.state, g_pal.ok, "%s", s_nt.ip);
            else sn_set(s_nt.state, g_pal.warn, "! NO SUCH NAME");
            snprintf(r, n, s_nt.ok ? "That is the address of %s." : "Nothing answers to %s.", s_nt.host);
            break;
        case JOB_TRACE:
            if (!s_nt.ok) {
                sn_set(s_nt.state, g_pal.warn, "! COULD NOT FIND IT");
                snprintf(r, n, "The name did not resolve, or a socket could not be opened.");
                break;
            }
            for (int i = 0; i < s_nt.hop_n && o < n - 60; i++) {
                const net_hop_t *h = &s_nt.hops[i];
                if (h->ip[0]) o += (size_t)snprintf(r + o, n - o, "%2d   %-15s  %6.1f ms%s\n", h->hop, h->ip, (double)h->ms, h->reached ? "   the destination" : "");
                else o += (size_t)snprintf(r + o, n - o, "%2d   no answer\n", h->hop);
            }
            if (s_nt.hop_n && s_nt.hops[s_nt.hop_n - 1].reached) sn_set(s_nt.state, g_pal.ok, "REACHED IN %d HOPS", s_nt.hop_n);
            else sn_set(s_nt.state, g_pal.warn, "! STOPPED BEFORE REACHING IT");
            break;
        case JOB_PORT:
            if (s_nt.ok) sn_set(s_nt.state, g_pal.ok, "PORT %d IS OPEN", s_nt.port);
            else sn_set(s_nt.state, g_pal.warn, "PORT %d IS CLOSED", s_nt.port);
            snprintf(r, n, s_nt.ok ? "Something is listening there. It answered in %.0f ms." : "Closed, or filtered on the way. Gave up after %.0f ms.", (double)s_nt.port_ms);
            break;
        default: break;
    }
    lv_label_set_text(s_nt.out, r);
    sn_action_enable(host_run, true);
}

static void host_run(void)
{
    if (s_nt.screen_job < 0 || !s_nt.host_ta) return;
    if (offline()) {
        sn_set(s_nt.state, g_pal.warn, "! %s", offline());
        lv_label_set_text(s_nt.out, "Join a Wi-Fi network in SETTINGS first.");
        return;
    }
    if (net_tools_busy()) {
        sn_set(s_nt.state, g_pal.warn, "ANOTHER TEST IS STILL RUNNING");
        return;
    }
    const char *t = lv_textarea_get_text(s_nt.host_ta);
    while (*t == ' ') t++;
    if (!*t) {
        sn_set(s_nt.state, g_pal.warn, "TYPE A HOST FIRST");
        lv_label_set_text(s_nt.out, "A name like example.com, or an address like 192.168.1.1.");
        return;
    }
    snprintf(s_nt.host, sizeof(s_nt.host), "%s", t);
    for (char *p = s_nt.host; *p; p++)
        if (*p == ' ' || *p == '/') {
            *p = 0;
            break;
        }
    if (s_nt.screen_job == JOB_PORT) {
        int port = atoi(lv_textarea_get_text(s_nt.port_ta));
        if (port < 1 || port > 65535) {
            sn_set(s_nt.state, g_pal.warn, "THE PORT MUST BE 1 TO 65535");
            return;
        }
        s_nt.port = port;
    }
    s_nt.job = s_nt.screen_job;
    sn_set(s_nt.state, g_pal.accent, "%s %s...", s_running[s_nt.job], s_nt.host);
    lv_label_set_text(s_nt.out, "");
    sn_action_enable(host_run, false);
    if (!net_tools_run(host_job, host_done, NULL)) {
        s_nt.job = -1;
        sn_set(s_nt.state, g_pal.warn, "! COULD NOT START");
        sn_action_enable(host_run, true);
    }
}

/* A field to type in: the one thing on these screens that is neither text nor a button. */
static lv_obj_t *field(lv_obj_t *row, const char *caption, const char *hint, const char *value, int32_t width, int max)
{
    lv_obj_t *cap = lv_label_create(row);
    lv_obj_set_style_text_font(cap, g_font.mono_b, 0);
    lv_obj_set_style_text_color(cap, g_pal.label, 0);
    lv_label_set_text(cap, caption);
    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, hint);
    lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_text(ta, value);
    lv_obj_set_width(ta, width);
    lv_obj_set_style_text_font(ta, g_font.mono_l, 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
    lv_obj_set_style_border_color(ta, lv_color_white(), 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 4, 0);
    lv_group_add_obj(deck_input_group(), ta);
    return ta;
}

static void host_build(lv_obj_t *body, int job)
{
    once();
    s_nt.screen_job = job;
    lv_obj_t *row   = sn_row_box(body);
    lv_obj_set_style_pad_column(row, 14, 0);
    s_nt.host_ta = field(row, "HOST", "name or address", s_nt.host, job == JOB_PORT ? 620 : 820, 90);
    s_nt.port_ta = NULL;
    if (job == JOB_PORT) {
        char port[8];
        snprintf(port, sizeof(port), "%d", s_nt.port);
        s_nt.port_ta = field(row, "PORT", "443", port, 150, 5);
        lv_textarea_set_accepted_chars(s_nt.port_ta, "0123456789");
    }
    lv_group_focus_obj(s_nt.host_ta);
    s_nt.state = sn_state(body);
    sn_set(s_nt.state, g_pal.label, "TYPE A HOST, THEN PRESS ENTER");
    lv_obj_t *box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    s_nt.out = lv_label_create(box);
    lv_obj_set_width(s_nt.out, LV_PCT(100));
    lv_obj_set_style_text_font(s_nt.out, g_font.mono_b, 0);
    lv_obj_set_style_text_color(s_nt.out, g_pal.text, 0);
    lv_obj_set_style_text_line_space(s_nt.out, 6, 0);
    lv_label_set_text(s_nt.out, job == JOB_PORT ? "Tab moves between the two fields." : "");
    sn_action(HID_ENTER, "ENTER", s_verb[job], host_run);
    if (s_nt.job >= 0) sn_action_enable(host_run, false);
}

static void ping_build(lv_obj_t *b) { host_build(b, JOB_PING); }
static void dns_build(lv_obj_t *b) { host_build(b, JOB_DNS); }
static void trace_build(lv_obj_t *b) { host_build(b, JOB_TRACE); }
static void port_build(lv_obj_t *b) { host_build(b, JOB_PORT); }

static void host_destroy(void)
{
    s_nt.screen_job = -1;
    s_nt.host_ta = s_nt.port_ta = s_nt.state = s_nt.out = NULL;
}

/* ---- LAN DEVICES ----------------------------------------------------------------- */

static void lan_progress(int done, int total, void *u)
{
    (void)u;
    s_nt.lan_done  = done;
    s_nt.lan_total = total;
}

static void lan_job(void *u)
{
    (void)u;
    s_nt.host_n = net_lan_scan(s_nt.hosts, HOSTS_MAX, lan_progress, NULL);
}

static void lan_scan(void);

static void lan_render(void)
{
    if (!s_nt.lan_up) return;
    if (s_nt.lan_detail) {
        const net_host_t *h = s_nt.lan_sel < s_nt.host_n ? &s_nt.hosts[s_nt.lan_sel] : NULL;
        if (!h) return;
        const char *vendor = h->has_mac ? net_oui_vendor(h->mac) : "";
        sn_set(s_nt.state, lv_color_white(), "%s", h->ip);
        sn_set(s_nt.v[0], h->self ? g_pal.ok : g_pal.text, "%s", h->self ? "This is the deck itself." : h->name[0] ? h->name : "It gives no name.");
        if (h->has_mac) sn_set(s_nt.v[1], g_pal.text, "ADDRESS  %02X:%02X:%02X:%02X:%02X:%02X", h->mac[0], h->mac[1], h->mac[2], h->mac[3], h->mac[4], h->mac[5]);
        else sn_set(s_nt.v[1], g_pal.text, "ADDRESS  NOT KNOWN");
        sn_set(s_nt.v[2], g_pal.text, "MAKER    %s", vendor[0] ? vendor : "NOT KNOWN");
        sn_set(s_nt.v[3], g_pal.text, "FOUND BY %s", h->service[0] ? h->service : h->ms > 0 ? "ping" : "its address being in use");
        return;
    }
    const char *off = offline();
    if (s_nt.lan_running) sn_set(s_nt.state, g_pal.accent, "LOOKING, %d %%", s_nt.lan_done * 100 / (s_nt.lan_total ? s_nt.lan_total : 1));
    else if (off) sn_set(s_nt.state, g_pal.warn, "! %s. Join one in SETTINGS.", off);
    else if (s_nt.lan_ran) sn_set(s_nt.state, g_pal.label, "%d DEVICE%s ANSWERED ON THIS NETWORK.", s_nt.host_n, s_nt.host_n == 1 ? "" : "S");
    else sn_set(s_nt.state, g_pal.label, "NOT LOOKED YET. IT TAKES ABOUT HALF A MINUTE.");
    sn_list_set(&s_nt.list, s_nt.lan_running ? 0 : s_nt.host_n, s_nt.list.sel, s_nt.lan_running ? "ASKING EVERY ADDRESS ON THIS NETWORK..." : "PRESS S TO LOOK");
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        int k = sn_list_item(&s_nt.list, r);
        if (k < 0) continue;
        const net_host_t *h      = &s_nt.hosts[k];
        const char       *vendor = h->has_mac ? net_oui_vendor(h->mac) : "";
        sn_set(sn_list_text(&s_nt.list, r), h->self ? g_pal.ok : g_pal.text, "%-15s  %-24.24s %s%s", h->ip, h->name[0] ? h->name : "-", vendor, h->self ? "  (THIS DECK)" : "");
    }
    sn_action_enable(lan_scan, !s_nt.lan_running && !off);
}

static void lan_done_cb(void *u)
{
    (void)u;
    s_nt.lan_running = false;
    s_nt.lan_ran     = true;
    lan_render();
}

static void lan_scan(void)
{
    if (s_nt.lan_running || net_tools_busy() || offline()) return;
    s_nt.lan_running = true;
    s_nt.lan_done = s_nt.lan_total = 0;
    s_nt.host_n = 0;
    if (!net_tools_run(lan_job, lan_done_cb, NULL)) s_nt.lan_running = false;
    lan_render();
}

static void lan_tick(lv_timer_t *t)
{
    (void)t;
    if (s_nt.lan_running) lan_render();
}

static void lan_list_build(void);

static void lan_open(int index)
{
    if (index < 0 || index >= s_nt.host_n) return;
    s_nt.lan_sel    = index;
    s_nt.lan_detail = true;
    lv_obj_t *body  = sn_body_reset();
    memset(&s_nt.list, 0, sizeof(s_nt.list));
    s_nt.state = sn_state(body);
    for (int i = 0; i < 4; i++) s_nt.v[i] = sn_value(body);
    lan_render();
}

static void lan_list_build(void)
{
    lv_obj_t *body  = sn_body_reset();
    s_nt.lan_detail = false;
    s_nt.state      = sn_note(body);
    lv_obj_set_style_text_font(s_nt.state, g_font.mono_b, 0);
    sn_list_build(&s_nt.list, body, lan_open);
    s_nt.list.sel = s_nt.lan_sel;
    sn_action(HID_LETTER('S'), "S", "LOOK FOR DEVICES", lan_scan);
    sn_help("Asks every address on the network the deck has joined whether anyone is there, and listens for devices that "
            "announce themselves by name. It takes about half a minute.\n\n"
            "A device that answers nothing does not appear. Use it on your own network.");
    lan_render();
}

static void lan_build(lv_obj_t *body)
{
    (void)body;
    once();
    s_nt.lan_up = true;
    lan_list_build();
    s_nt.timer = lv_timer_create(lan_tick, 500, NULL);
}

static void lan_destroy(void)
{
    if (s_nt.timer) lv_timer_delete(s_nt.timer);
    s_nt.timer  = NULL;
    s_nt.lan_up = false;
    s_nt.state  = NULL;
    memset(&s_nt.list, 0, sizeof(s_nt.list));
}

static bool lan_back(void)
{
    if (!s_nt.lan_detail) return false;
    lan_list_build();
    return true;
}

static bool lan_key(const deck_key_t *k)
{
    if (s_nt.lan_detail || !sn_list_key(&s_nt.list, k)) return false;
    if (!s_nt.lan_detail) {
        s_nt.lan_sel = s_nt.list.sel;
        lan_render();
    }
    return true;
}

/* ---- SPEED TEST -------------------------------------------------------------------- */

static void speed_job(void *u)
{
    (void)u;
    s_nt.spd_phase   = 1;
    s_nt.spd_lat_ok  = net_port_open(SPEED_HOST, 443, 3000, &s_nt.spd_lat_ms);
    s_nt.spd_phase   = 2;
    s_nt.spd_down_ok = net_speed_download(SPEED_DOWN_URL, SPEED_DOWN_BYTES, 20000, &s_nt.spd_down_bytes, &s_nt.spd_down_ms);
    s_nt.spd_phase   = 3;
    s_nt.spd_up_ok   = net_speed_upload(SPEED_UP_URL, SPEED_UP_BYTES, 20000, &s_nt.spd_up_ms);
    s_nt.spd_phase   = 4;
}

static void mbps(char *out, size_t n, size_t bytes, float ms, bool ok)
{
    if (!ok || ms <= 0) {
        snprintf(out, n, "--");
        return;
    }
    double v = (double)bytes * 8.0 / (ms / 1000.0) / 1e6;
    snprintf(out, n, v >= 100 ? "%.0f" : "%.1f", v);
}

static void speed_start(void);

static void speed_render(void)
{
    if (!s_nt.speed_up) return;
    static const char *const names[] = {"", "MEASURING THE DELAY...", "DOWNLOADING 8 MB...", "UPLOADING 2 MB..."};
    int         ph  = s_nt.spd_phase;
    const char *off = offline();
    bool        busy = ph >= 1 && ph <= 3;
    if (busy) sn_set(s_nt.state, g_pal.accent, "%s", names[ph]);
    else if (off) sn_set(s_nt.state, g_pal.warn, "! %s. Join one in SETTINGS.", off);
    else if (s_nt.spd_ran && !s_nt.spd_down_ok) sn_set(s_nt.state, g_pal.warn, "! THE TEST SERVER DID NOT ANSWER.");
    else if (s_nt.spd_ran) sn_set(s_nt.state, g_pal.label, "DONE. 8 MB DOWN AND 2 MB UP WERE SENT TO MEASURE IT.");
    else sn_set(s_nt.state, g_pal.label, "NOT MEASURED YET. THE TEST USES ABOUT 10 MB OF DATA.");
    char d[16] = "--", u[16] = "--", l[16] = "--";
    if (s_nt.spd_ran && !busy) {
        mbps(d, sizeof(d), s_nt.spd_down_bytes, s_nt.spd_down_ms, s_nt.spd_down_ok);
        mbps(u, sizeof(u), SPEED_UP_BYTES, s_nt.spd_up_ms, s_nt.spd_up_ok);
        if (s_nt.spd_lat_ok) snprintf(l, sizeof(l), "%.0f", (double)s_nt.spd_lat_ms);
    } else if (busy) {
        snprintf(d, sizeof(d), "..."), snprintf(u, sizeof(u), "..."), snprintf(l, sizeof(l), "...");
    }
    sn_set(s_nt.v[0], lv_color_white(), "%s", d);
    sn_set(s_nt.v[1], lv_color_white(), "%s", u);
    sn_set(s_nt.v[2], lv_color_white(), "%s", l);
    sn_action_enable(speed_start, !busy && !off);
}

static void speed_done(void *u)
{
    (void)u;
    s_nt.spd_phase = 0;
    s_nt.spd_ran   = true;
    speed_render();
}

static void speed_start(void)
{
    if (s_nt.spd_phase != 0 || net_tools_busy() || offline()) return;
    s_nt.spd_phase = 1;
    if (!net_tools_run(speed_job, speed_done, NULL)) s_nt.spd_phase = 0;
    speed_render();
}

static void speed_tick(lv_timer_t *t)
{
    (void)t;
    speed_render();
}

static lv_obj_t *speed_cell(lv_obj_t *parent, const char *title, const char *unit)
{
    lv_obj_t *c = sn_col(parent, 0);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_t *t = lv_label_create(c);
    lv_obj_set_style_text_font(t, g_font.mono_b, 0);
    lv_obj_set_style_text_color(t, g_pal.label, 0);
    lv_label_set_text(t, title);
    lv_obj_t *v = lv_label_create(c);
    lv_obj_set_style_text_font(v, g_font.disp_hero, 0);
    lv_label_set_text(v, "--");
    lv_obj_t *un = lv_label_create(c);
    lv_obj_set_style_text_font(un, g_font.mono_b, 0);
    lv_obj_set_style_text_color(un, g_pal.label, 0);
    lv_label_set_text(un, unit);
    return v;
}

static void speed_build(lv_obj_t *body)
{
    once();
    s_nt.speed_up = true;
    s_nt.state    = sn_note(body);
    lv_obj_set_style_text_font(s_nt.state, g_font.mono_b, 0);
    lv_obj_t *cells = sn_row_box(body);
    lv_obj_set_style_margin_top(cells, 30, 0);
    s_nt.v[0] = speed_cell(cells, "DOWNLOAD", "MEGABITS A SECOND");
    s_nt.v[1] = speed_cell(cells, "UPLOAD", "MEGABITS A SECOND");
    s_nt.v[2] = speed_cell(cells, "DELAY", "MILLISECONDS TO CONNECT");
    sn_action(HID_ENTER, "ENTER", "START THE TEST", speed_start);
    sn_help("Downloads 8 MB over plain HTTP (so the number is the connection, not the deck decrypting) and uploads 2 MB "
            "to Cloudflare. DELAY is how long it takes to open a connection to the test server.\n\n"
            "It measures the whole path: your Wi-Fi, your router and your provider.");
    s_nt.timer = lv_timer_create(speed_tick, 500, NULL);
    speed_render();
}

static void speed_destroy(void)
{
    if (s_nt.timer) lv_timer_delete(s_nt.timer);
    s_nt.timer    = NULL;
    s_nt.speed_up = false;
    s_nt.state    = NULL;
}

/* ---- The menu ------------------------------------------------------------------------ */

static void menu_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)st;
    (void)level;
    if (net_state() == NET_CONNECTED) snprintf(line, n, "JOINED: %s\n%s", net_ssid(), net_ip());
    else snprintf(line, n, "NOT JOINED TO A NETWORK");
}

static const sn_screen_t s_ping  = {.name = "PING", .purpose = "Does a host answer, and how fast?", .icon = ICON_REFRESH, .build = ping_build, .destroy = host_destroy};
static const sn_screen_t s_dns   = {.name = "DNS", .purpose = "What address does a name have?", .icon = ICON_SERVER, .build = dns_build, .destroy = host_destroy};
static const sn_screen_t s_trace = {.name = "TRACEROUTE", .purpose = "The path packets take to a host.", .icon = ICON_LAN, .build = trace_build, .destroy = host_destroy};
static const sn_screen_t s_port  = {.name = "PORT CHECK", .purpose = "Is a port open on a host?", .icon = ICON_PLUG, .build = port_build, .destroy = host_destroy};
static const sn_screen_t s_lan   = {.name = "LAN DEVICES", .purpose = "Who else is on the network you joined.", .icon = ICON_LAN, .build = lan_build, .destroy = lan_destroy, .key = lan_key, .back = lan_back};
static const sn_screen_t s_speed = {.name = "SPEED TEST", .purpose = "How fast your connection is.", .icon = ICON_GAUGE, .build = speed_build, .destroy = speed_destroy};

static const sn_screen_t *const s_kids[] = {&s_ping, &s_dns, &s_trace, &s_port, &s_lan, &s_speed};

const sn_screen_t sn_net_menu = {
    .name    = "NETWORK",
    .purpose = "Test the network the deck has joined: hosts, devices, speed.",
    .icon    = ICON_LAN,
    .kids    = s_kids,
    .nkids   = 6,
    .status  = menu_status,
};
