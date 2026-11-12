/*
 * deck_sentinel for the simulator: a pretend Sentinel board. It composes
 * the same $RFS sentences the C5 sends and pushes them through the real
 * parser, so RF WATCH in the sim exercises everything but the UART.
 *
 * The scene: a quiet 433 MHz band with a key fob chirping on 433.9 MHz now
 * and then, 2.4 GHz busy where Wi-Fi channels 1, 6 and 11 sit, a GPS that
 * finds a 3D fix after a few seconds, and the networks net_sim.c invents.
 *
 * The pretend node follows protocol 2 the way main/main.cpp in rf-sentinel
 * does: it answers BAUD, MODE and ARM, lets an arm lease run out, refuses
 * transmit tests without one, and sends a captured beacon every few seconds
 * so the capture file path is exercised. The host side is the real link
 * machine (sentinel_host), and events and frames go to the real store.
 *
 * Bluetooth: a room of pretend advertisers (phones, headphones, a light, an
 * AirTag away from its owner, a Tile, a SmartTag, and later an Axon camera
 * and a Flock battery) whose hand-built advertisements go through the same
 * table the C5 uses (sentinel_bleagg), so the BDV rows, the labels and the
 * track watch are the real code. MODE,ble and CAP work as on the node.
 *
 * Wi-Fi: the same idea. Hand-built beacons for the scene's access points go
 * through the node's monitor (sentinel_wifimon), so the WBS rows and the
 * detectors are the real code. A minute in, someone deauths Afterlife, an
 * open copy of NIGHTCITY-5G appears, and a drone flies past broadcasting
 * its Remote ID. In MODE,wifi the monitor hops and reports each dwell; the
 * honeypot answers MODE,hpot only while the pretend node is armed.
 */
#include "deck_sentinel.h"
#include "sentinel_drivelog.h"
#include "sentinel_bleagg.h"
#include "sentinel_cmd.h"
#include "sentinel_host.h"
#include "sentinel_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

static sentinel_state_t s_state;
static sentinel_lines_t s_lines;
static lv_timer_t      *s_timer;
static bool             s_on;
static uint32_t         s_started;
static uint32_t         s_rng = 0x5EED1E55u;
static int              s_tick;
static bool             s_quiet;
static sentinel_host_t  s_host;     /* the deck's side of the link */
static sentinel_arm_t   s_node_arm; /* the node's lease */
static bool             s_node_armed_shown;
static sentinel_bleagg_t  s_node_ble;  /* the node's Bluetooth table */
static sentinel_blehost_t s_ble;       /* the deck's: devices and the track watch */
static bool               s_node_ble_mode, s_node_cap;
static sentinel_wifimon_t  s_node_wifi; /* the node's Wi-Fi monitor */
static sentinel_wifihost_t s_wifi;      /* the deck's tables */
static sentinel_bandhost_t s_band, s_band_site; /* the deck's: band history, bursts, the jam watch */
static sentinel_subwatch_t s_node_sub;  /* the node's band watches */
static sentinel_nrfwatch_t s_node_nrf;
static uint32_t s_node_dwell_khz;       /* where the node's CC1101 is parked, 0 while it sweeps */
static int      s_dwell_tick, s_dwell_bursts;
static uint32_t s_dwell_last_ms;
static enum { NODE_WATCH, NODE_BLE, NODE_WIFI, NODE_HPOT, NODE_DRIVE } s_node_tool;
static double  s_street_m;              /* how far up the street a wardrive has carried the deck */
static uint32_t s_drive_lost_base;
static int     s_hop;                   /* the slot the pretend radio is on */
static uint8_t s_hp_ch;
static char    s_hp_ssid[SENTINEL_WIFI_SSID_MAX + 1];
static int     s_hp_tick, s_hp_joins, s_hp_probes;

uint32_t sentinel_now_ms(void) { return lv_tick_get(); }

static int  s_jam_at = -1; /* the jam scene: see emit_nrf() */
static bool jammed(void) { return s_jam_at >= 0 && (int)((lv_tick_get() - s_started) / 1000) >= s_jam_at; }

static uint32_t rnd(uint32_t n)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (s_rng >> 8) % n;
}

/* No writer task here: the sink writes straight to the store. */
static void sink_event(void *user, const sentinel_event_t *e)
{
    (void)user;
    sentinel_store_event(e, s_state.fix, s_state.lat, s_state.lon);
}
static void sink_packet(void *user, const sentinel_pkt_t *p)
{
    (void)user;
    sentinel_store_packet(p);
}
static const sentinel_sink_t s_sink;
/* A wardrive's rows go straight to the store too. */
static bool drive_row(sentinel_drive_file_t file, const char *row, int len)
{
    sentinel_store_drive_row(file, row, (size_t)len);
    return true;
}
static void sink_device(void *user, const sentinel_bdv_t *d)
{
    (void)user;
    sentinel_blehost_device(&s_ble, &s_state, d, sentinel_now_ms(), &s_sink);
    sentinel_drivelog_device(&s_state, d, sentinel_now_ms(), drive_row);
}
static void sink_channel(void *user, const sentinel_wch_t *c)
{
    (void)user;
    sentinel_wifihost_channel(&s_wifi, c);
}
static void sink_ap(void *user, const sentinel_wbs_t *a)
{
    (void)user;
    sentinel_wifihost_ap(&s_wifi, a, sentinel_now_ms());
    sentinel_drivelog_ap(&s_state, a, sentinel_now_ms(), drive_row);
}
static void sink_drone(void *user, const sentinel_drn_t *d)
{
    (void)user;
    sentinel_wifihost_drone(&s_wifi, &s_state, d, sentinel_now_ms(), &s_sink);
}
static void sink_burst(void *user, const sentinel_brs_t *b)
{
    (void)user;
    if (b->khz == s_host.want_dwell_khz) sentinel_bursthost_add(&s_band.bursts, b, sentinel_now_ms());
}
static const sentinel_sink_t s_sink = {
    .event = sink_event, .packet = sink_packet, .device = sink_device, .channel = sink_channel, .ap = sink_ap, .drone = sink_drone,
    .burst = sink_burst,
};

static void emit(const char *payload)
{
    char frame[SENTINEL_LINE_MAX + 8];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    if (n > 0) sentinel_proto_feed(&s_lines, &s_state, frame, (size_t)n, sentinel_now_ms(), &s_sink);
}

/* ---- The pretend room, on Bluetooth ---------------------------------------- */

typedef struct {
    uint8_t addr[6];
    bool    pub;
    int8_t  rssi;
    int     from_tick; /* walks into earshot at this tick */
    uint8_t adv[SENTINEL_BLE_ADV_MAX];
    uint8_t len;
} sim_ble_t;

#define APPLE(...) {2, 0x01, 0x1A, __VA_ARGS__}
static sim_ble_t s_room[] = {
    {{0x5A, 0x11, 0x0C, 0x33, 0x9E, 0x01}, false, -48, 0, APPLE(10, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x01, 0x18, 0x2A, 0x3B, 0x4C), 14},
    {{0x6B, 0x47, 0xD2, 0x90, 0x12, 0x02}, false, -63, 0, APPLE(10, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x03, 0x1C, 0x5D, 0x6E, 0x7F), 14},
    {{0xAC, 0x80, 0x0A, 0x41, 0x7C, 0x03}, true, -55, 0, {2, 0x01, 0x06, 11, 0x09, 'W', 'H', '-', '1', '0', '0', '0', 'X', 'M', '5', 5, 0xFF, 0x2D, 0x01, 0x04, 0x00}, 21},
    {{0xD4, 0xAD, 0xFC, 0x08, 0x4A, 0x53}, true, -70, 0, {2, 0x01, 0x06, 12, 0x09, 'G', 'o', 'v', 'e', 'e', '_', 'H', '6', '0', '4', '6'}, 16},
    /* an AirTag away from its owner: Find My, 25 bytes, status says AirTag */
    {{0xE1, 0x7C, 0x55, 0x2B, 0x90, 0xA6}, false, -58, 4, {30, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10, 0x8E, 0x21, 0x5F, 0x02, 0x6D, 0xB3, 0x47, 0x90, 0x1C, 0xE5, 0x38, 0x7A, 0x04, 0xD9, 0x66, 0x2F, 0xA1, 0x53, 0x0B, 0xC8, 0x72, 0x9D, 0x01, 0x4E}, 31},
    {{0xF3, 0x02, 0x9A, 0x6E, 0x18, 0xC4}, false, -72, 0, {2, 0x01, 0x06, 3, 0x03, 0xED, 0xFE}, 7},
    /* someone's AirPods case, owner near: the short Find My entry */
    {{0x4D, 0x39, 0xB7, 0x05, 0xEE, 0x21}, false, -60, 0, APPLE(7, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x24, 0x02), 11},
    {{0x7E, 0x93, 0x10, 0xC2, 0x5D, 0x88}, false, -80, 8, {2, 0x01, 0x06, 7, 0x16, 0x5A, 0xFD, 0x13, 0x00, 0x00, 0x01}, 11},
    {{0x00, 0x25, 0xDF, 0x4A, 0x1B, 0x7E}, true, -74, 40, {2, 0x01, 0x06}, 3},
    {{0xC8, 0x2E, 0x47, 0x91, 0x0D, 0x35}, false, -77, 80, {2, 0x01, 0x06, 15, 0x09, 'F', 'S', ' ', 'E', 'x', 't', ' ', 'B', 'a', 't', 't', 'e', 'r', 'y'}, 19},
    {{0x51, 0xBE, 0x04, 0x7A, 0x66, 0x19}, false, -84, 0, {2, 0x01, 0x06, 5, 0xFF, 0x06, 0x00, 0x01, 0x09}, 9},
    {{0x62, 0x0F, 0xE8, 0x33, 0x21, 0x4B}, false, -88, 0, {2, 0x01, 0x06, 5, 0xFF, 0x75, 0x00, 0x42, 0x04}, 9},
    {{0x48, 0xD7, 0x1C, 0xA9, 0x05, 0x77}, false, -91, 0, {2, 0x01, 0x04}, 3},
};
#define ROOM_N ((int)(sizeof(s_room) / sizeof(s_room[0])))

/* One quarter second of the room: who spoke, and what the node makes of it. */
static void ble_tick(void)
{
    uint32_t now = sentinel_now_ms();
    for (int i = 0; i < ROOM_N; i++) {
        sim_ble_t *d = &s_room[i];
        if (s_tick < d->from_tick || rnd(3) == 0) continue; /* not here yet, or not heard this time */
        if (jammed() && rnd(15) != 0) continue;             /* the jam scene: hardly any advertisement gets through */
        int8_t rssi = (int8_t)(d->rssi - 3 + (int)rnd(7));
        sentinel_bleagg_feed(&s_node_ble, d->addr, d->pub, rssi, d->adv, d->len, now);
        if (s_node_cap && s_node_tool == NODE_BLE) { /* the same advertisement, as a captured packet */
            sentinel_pkt_t p = {.link = SENTINEL_LINK_BLE, .ch = 37, .rssi = rssi, .us = (lv_tick_get() - s_started) * 1000u + (uint32_t)i};
            p.len = p.orig_len = (uint16_t)sentinel_ble_ll_frame(p.data, sizeof(p.data), d->addr, d->pub, 0, d->adv, d->len);
            char payload[SENTINEL_LINE_MAX];
            if (p.len && sentinel_proto_pkt(payload, sizeof(payload), &p)) emit(payload);
        }
    }
    sentinel_bdv_t row;
    char           payload[96];
    for (int i = 0; i < 4 && sentinel_bleagg_next(&s_node_ble, now, &row); i++)
        if (sentinel_proto_bdv(payload, sizeof(payload), &row)) emit(payload);
    if (s_tick % 8 == 3) {
        sentinel_ble_summary_t sum;
        sentinel_bleagg_summary(&s_node_ble, now, &sum);
        snprintf(payload, sizeof(payload), "BLE,%u,%u,%u,%d", sum.devices, sum.rate, sum.trackers, sum.strongest);
        emit(payload);
    }
}

/* ---- The pretend room, on Wi-Fi --------------------------------------------- */

typedef struct {
    uint8_t     bssid[6];
    const char *ssid;
    uint8_t     ch;
    int8_t      rssi;
    int         sec;       /* 0 open, 3 WPA2, 4 WPA3 */
    int         from_tick; /* appears at this tick */
} sim_ap_t;

static const sim_ap_t s_aps[] = {
    {{0xA4, 0xC3, 0xF0, 0x11, 0x22, 0x33}, "NIGHTCITY-5G", 1, -41, 3, 0},
    {{0xA4, 0xC3, 0xF0, 0x11, 0x22, 0x34}, "ARASAKA-GUEST", 6, -52, 0, 0},
    {{0x3C, 0x22, 0xFB, 0x44, 0x55, 0x66}, "Afterlife", 11, -63, 3, 0},
    {{0x3C, 0x22, 0xFB, 0x44, 0x55, 0x67}, "", 11, -70, 4, 0},
    {{0xF0, 0xB4, 0xD2, 0x77, 0x88, 0x99}, "netwatch_honeypot", 6, -77, 3, 0},
    {{0xF0, 0xB4, 0xD2, 0x77, 0x88, 0x9A}, "Kiroshi, Optics", 1, -84, 3, 0},
    {{0xA4, 0xC3, 0xF0, 0x11, 0x22, 0x40}, "NIGHTCITY-5G", 36, -55, 3, 0},
    {{0x3C, 0x22, 0xFB, 0x44, 0x55, 0x70}, "Afterlife", 149, -68, 3, 0},
    {{0x9C, 0x1E, 0x95, 0x02, 0x03, 0x04}, "Militech-IoT", 44, -79, 4, 0},
    {{0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x01}, "NIGHTCITY-5G", 1, -47, 0, 240}, /* the open twin */
};
#define APS_N ((int)(sizeof(s_aps) / sizeof(s_aps[0])))

/* A beacon for one of them, as it would come off the air. */
static size_t sim_beacon(uint8_t *f, const sim_ap_t *a)
{
    size_t n = 0;
    static const uint8_t head[10] = {0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(f, head, 10), n = 10;
    memcpy(f + n, a->bssid, 6), n += 6;
    memcpy(f + n, a->bssid, 6), n += 6;
    f[n++] = 0x10, f[n++] = 0;
    memset(f + n, 0, 8), n += 8;
    f[n++] = 0x64, f[n++] = 0, f[n++] = (uint8_t)(0x01 | (a->sec ? 0x10 : 0)), f[n++] = 0;
    size_t sl = strlen(a->ssid);
    f[n++] = 0, f[n++] = (uint8_t)sl;
    memcpy(f + n, a->ssid, sl), n += sl;
    f[n++] = 3, f[n++] = 1, f[n++] = a->ch;
    if (a->sec) { /* an RSN element: WPA2 personal, or WPA3 with protected management frames required */
        const uint8_t rsn[] = {48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, (uint8_t)(a->sec == 4 ? 8 : 2), (uint8_t)(a->sec == 4 ? 0xC0 : 0), 0};
        memcpy(f + n, rsn, sizeof(rsn)), n += sizeof(rsn);
    }
    return n;
}

/* What the node's monitor makes of one frame: alerts and drones out, the
 * frame itself too when capturing. */
static void wifi_frame(const uint8_t *f, size_t n, uint8_t ch, int8_t rssi)
{
    sentinel_wifimon_out_t out;
    char                   line[160];
    sentinel_wifimon_frame(&s_node_wifi, f, n, ch, rssi, sentinel_now_ms(), &out);
    if (out.alert) {
        snprintf(line, sizeof(line), "EVT,warn,%s,%s", out.a.src, out.a.text);
        emit(line);
    }
    if (out.drone && sentinel_proto_drn(line, sizeof(line), &out.d)) emit(line);
    if (s_node_cap && s_node_tool == NODE_WIFI) {
        sentinel_pkt_t p = {.link = SENTINEL_LINK_WIFI, .ch = ch, .rssi = rssi, .us = (lv_tick_get() - s_started) * 1000u};
        p.orig_len = (uint16_t)n;
        p.len      = (uint16_t)(n > SENTINEL_PKT_MAX ? SENTINEL_PKT_MAX : n);
        memcpy(p.data, f, p.len);
        char payload[SENTINEL_LINE_MAX]; /* a captured frame needs the long buffer */
        if (sentinel_proto_pkt(payload, sizeof(payload), &p)) emit(payload);
    }
}

/* Everything on one channel for one dwell. */
static void wifi_dwell(uint8_t ch)
{
    uint8_t f[200];
    for (int i = 0; i < APS_N; i++) {
        if (s_aps[i].ch != ch || s_tick < s_aps[i].from_tick) continue;
        wifi_frame(f, sim_beacon(f, &s_aps[i]), ch, (int8_t)(s_aps[i].rssi - 2 + (int)rnd(5)));
    }
    /* On a wardrive the deck moves up a street with an access point every
     * 15 m, each in earshot for 40 m either side: the room falls behind and
     * new networks keep coming. One of them asks not to be mapped. */
    if (s_node_tool == NODE_DRIVE) {
        static const uint8_t chans[] = {1, 6, 11, 36, 44, 149};
        for (int k = (int)((s_street_m - 40) / 15); k <= (int)((s_street_m + 40) / 15); k++) {
            if (k < 1 || chans[k % 6] != ch) continue;
            char     name[24];
            sim_ap_t a = {{0x10, 0x5A, 0xB1, 0x00, (uint8_t)(k >> 8), (uint8_t)k}, name, ch, 0, k % 5 == 0 ? 0 : k % 3 ? 3 : 4, 0};
            snprintf(name, sizeof(name), k == 7 ? "Corpo-Plaza_nomap" : "STREET-%03d", k);
            double off = s_street_m - k * 15.0;
            wifi_frame(f, sim_beacon(f, &a), ch, (int8_t)(-45 - (int)((off < 0 ? -off : off) * 0.9) - (int)rnd(4)));
        }
    }
    /* someone knocks Afterlife's clients off, a minute in, for a few seconds */
    if (ch == 11 && s_tick >= 240 && s_tick < 264) {
        uint8_t d[26] = {0xC0, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        memcpy(d + 10, s_aps[2].bssid, 6), memcpy(d + 16, s_aps[2].bssid, 6);
        d[24] = 7;
        for (int k = 0; k < 14; k++) wifi_frame(d, sizeof(d), 11, -58);
    }
    /* a drone on channel 6: a beacon carrying its Remote ID, drifting north */
    if (ch == 6 && s_tick >= 280 && s_tick < 520) {
        static const sim_ap_t drone = {{0x60, 0x60, 0x1F, 0xAA, 0xBB, 0xCC}, "RID-1581F5FKD229", 6, -66, 0, 0};
        size_t  n = sim_beacon(f, &drone);
        int32_t lat = 407430000 + (s_tick - 280) * 300, lon = -739880000;
        uint8_t ie[60] = {221, 58, 0xFA, 0x0B, 0xBC, 0x0D, (uint8_t)s_tick, 0xF2, 25, 2, 0x02, 0x12};
        memcpy(ie + 12, "1581F5FKD229N00A12X", 19);
        uint8_t *loc = ie + 35;
        loc[0] = 0x12, loc[1] = 0x20, loc[3] = 48;
        for (int k = 0; k < 4; k++) loc[5 + k] = (uint8_t)((uint32_t)lat >> (8 * k)), loc[9 + k] = (uint8_t)((uint32_t)lon >> (8 * k));
        loc[15] = (uint8_t)2240, loc[16] = (uint8_t)(2240 >> 8); /* 120 m */
        memcpy(f + n, ie, 60), n += 60; /* the element: 2 + 5 + a pack of two 25-byte messages */
        wifi_frame(f, n, 6, -66);
    }
}

static void wifi_tick(void)
{
    char line[160];
    if (s_node_tool == NODE_WIFI || s_node_tool == NODE_DRIVE) {
        /* hop: three dwells a tick, so a sweep of all 38 channels takes about three seconds */
        for (int k = 0; k < 3; k++) {
            uint8_t ch = sentinel_wifi_channel[s_hop];
            wifi_dwell(ch);
            s_hop = (s_hop + 1) % SENTINEL_WIFI_CHANNELS;
            sentinel_wch_t done;
            sentinel_wifimon_dwell(&s_node_wifi, sentinel_wifi_channel[s_hop], ch <= 13 ? 120 : 80, &done);
            /* the traffic the monitor does not look inside: busy where the busy networks are */
            bool busy = ch == 1 || ch == 6 || ch == 11 || ch == 36 || ch == 149;
            done.data = (uint16_t)(busy ? 30 + rnd(ch == 6 ? 500 : 120) : rnd(4) == 0 ? rnd(5) : 0);
            done.ctrl = (uint16_t)(done.data ? done.data + rnd(20) : 0);
            if (done.data && done.max_rssi < -80) done.max_rssi = (int8_t)(-80 + (int)rnd(20));
            if (s_node_tool == NODE_WIFI && sentinel_proto_wch(line, sizeof(line), &done)) emit(line); /* a drive has no waterfall */
        }
    } else if (s_node_tool == NODE_WATCH && s_tick % 60 == 8) {
        /* the presence scan passes every channel once: the detectors get a slow sweep */
        for (int i = 0; i < SENTINEL_WIFI_CHANNELS; i++) wifi_dwell(sentinel_wifi_channel[i]);
    } else if (s_node_tool == NODE_HPOT) {
        s_hp_tick++;
        if (s_hp_tick % 3 == 0) s_hp_probes += 1 + (int)rnd(3);
        if (s_hp_tick == 24) { /* six seconds in, something takes the bait */
            s_hp_joins++;
            emit("EVT,warn,hpot,5A:11:0C:33:9E:01 joined the honeypot");
        }
        if (s_hp_tick % 8 == 1) {
            snprintf(line, sizeof(line), "HPT,%u,%d,%d,%d,%s", (unsigned)s_hp_ch, s_hp_joins ? 1 : 0, s_hp_joins, s_hp_probes, s_hp_ssid);
            emit(line);
        }
    }
    sentinel_wbs_t ap;
    for (int i = 0; i < (s_node_tool == NODE_DRIVE ? 10 : 4) && sentinel_wifimon_next(&s_node_wifi, sentinel_now_ms(), &ap); i++)
        if (sentinel_proto_wbs(line, sizeof(line), &ap)) emit(line);
}

static void node_capture(bool on)
{
    if (s_node_cap == on) return;
    s_node_cap = on;
    emit(on ? "CAP,1" : "CAP,0");
    printf("[sim] sentinel capture %d\n", on);
}

/* The lease as the node sees it: say so when it runs out, and show changes. */
static void node_arm_tick(void)
{
    uint32_t now = sentinel_now_ms();
    if (sentinel_arm_expire(&s_node_arm, now)) {
        emit("ARM,0,0");
        emit("EVT,info,node,disarmed: lease expired");
    }
    bool armed = sentinel_arm_active(&s_node_arm, now);
    if (armed != s_node_armed_shown) {
        s_node_armed_shown = armed;
        printf("[sim] sentinel arm %d\n", armed);
    }
    if (!armed && s_node_tool == NODE_HPOT) { /* no lease, no access point */
        s_node_tool = NODE_WATCH;
        emit("MODE,watch");
        emit("EVT,info,hpot,honeypot stopped");
        printf("[sim] sentinel mode watch\n");
    }
}

/* The jam scene, for screenshots and for watching the jam watch work:
 * SIM_RF_JAM=<seconds> saturates 2.4 GHz from then on and takes Bluetooth
 * down with it, the way a real jammer would. Nothing here transmits; it is
 * a simulator. */

/* What the node's band watch makes of a sweep: its events, and a BND row when the band changes. */
static void say_band(const sentinel_bw_out_t *o, bool nrf)
{
    char line[96];
    for (int i = 0; i < o->n; i++) {
        snprintf(line, sizeof(line), "EVT,%s,%s,%s", o->a[i].warn ? "warn" : "info", o->a[i].src, o->a[i].text);
        emit(line);
    }
    if (!o->band_changed) return;
    if (nrf ? sentinel_proto_bnd(line, sizeof(line), true, s_node_nrf.state, s_node_nrf.channels, 0)
            : sentinel_proto_bnd(line, sizeof(line), false, s_node_sub.state, s_node_sub.channels, s_node_sub.delta_db))
        emit(line);
}

static void emit_sub(void)
{
    char   buf[SENTINEL_LINE_MAX];
    int8_t v[SENTINEL_SUB_CH];
    int    n = snprintf(buf, sizeof(buf), "SUB,%d", SENTINEL_SUB_CH);
    bool fob = (s_tick % 160) < 6; /* a remote: 1.5 s every 40 s */
    for (int i = 0; i < SENTINEL_SUB_CH; i++) {
        int x = -108 + (int)rnd(5);
        if (i == 9 && fob) x = -58 - (int)rnd(6);            /* 433.9 MHz */
        if ((i == 8 || i == 10) && fob) x = -104 + (int)rnd(4); /* its skirts, too weak to count */
        v[i] = (int8_t)x;
        n += snprintf(buf + n, sizeof(buf) - n, ",%d", x);
    }
    sentinel_bw_out_t o;
    sentinel_subwatch_feed(&s_node_sub, v, &o);
    say_band(&o, false);
    if (!s_quiet) emit(buf);
}

static void emit_nrf(void)
{
    char    buf[SENTINEL_LINE_MAX];
    uint8_t v[SENTINEL_NRF_CH];
    int     n = snprintf(buf, sizeof(buf), "NRF,%d,", SENTINEL_NRF_CH);
    for (int i = 0; i < SENTINEL_NRF_CH; i++) {
        int mhz = SENTINEL_NRF_FIRST_MHZ + i, hits = rnd(10) == 0 ? 1 : 0;
        if (mhz >= 2402 && mhz <= 2422) hits = 2 + (int)rnd(4); /* channel 1 */
        if (mhz >= 2427 && mhz <= 2447) hits = 1 + (int)rnd(3); /* channel 6 */
        if (mhz >= 2452 && mhz <= 2472) hits = 4 + (int)rnd(5); /* channel 11 */
        if (mhz == 2480) hits = 1 + (int)rnd(3);                /* a BLE advertiser */
        if (jammed() && mhz <= 2500) hits = 7 + (int)rnd(2);
        v[i]     = (uint8_t)hits;
        buf[n++] = "0123456789ABCDEF"[hits & 0xF];
    }
    buf[n] = 0;
    sentinel_bw_out_t o;
    sentinel_nrfwatch_feed(&s_node_nrf, v, &o);
    say_band(&o, true);
    if (!s_quiet) emit(buf);
}

/* The burst listener: a sensor at 433.92 MHz every seven seconds, and a remote pressed twice. */
static void dwell_tick(void)
{
    char line[64];
    bool near = s_node_dwell_khz >= 433820 && s_node_dwell_khz <= 434020;
    uint32_t now = sentinel_now_ms();
    sentinel_brs_t b = {.khz = s_node_dwell_khz};
    if (near && s_dwell_tick % 28 == 6) b.ms = 9, b.peak = (int8_t)(-71 + (int)rnd(3) - 1);
    if (near && (s_dwell_tick == 40 || s_dwell_tick == 49)) b.ms = 600 + rnd(30), b.peak = -48;
    if (b.ms) {
        b.gap_ms        = s_dwell_bursts ? now - s_dwell_last_ms - b.ms : 0;
        s_dwell_last_ms = now;
        s_dwell_bursts++;
        if (sentinel_proto_brs(line, sizeof(line), &b)) emit(line);
    }
    if (s_dwell_tick % 8 == 0) {
        snprintf(line, sizeof(line), "SBD,%lu,%d,%d", (unsigned long)s_node_dwell_khz, -103 - (int)rnd(3), s_dwell_bursts);
        emit(line);
    }
    s_dwell_tick++;
}

static void emit_wifi(void)
{
    emit("WIFI,23,14,9,-41");
    emit("WAP,A4C3F0112233,1,-41,NIGHTCITY-5G");
    emit("WAP,A4C3F0112234,6,-52,ARASAKA-GUEST");
    emit("WAP,3C22FB445566,11,-63,Afterlife");
    emit("WAP,3C22FB445567,11,-70,<hidden>");
    emit("WAP,F0B4D2778899,6,-77,netwatch_honeypot");
    emit("WAP,F0B4D277889A,1,-84,Kiroshi, Optics"); /* a comma in an SSID */
}

static void tick(lv_timer_t *t)
{
    (void)t;
    uint32_t up = (lv_tick_get() - s_started) / 1000;
    /* The deck's link machine runs whether or not the node is up yet. */
    sentinel_host_out_t out;
    sentinel_host_tick(&s_host, &s_state, sentinel_now_ms(), &out);
    sentinel_drivelog_tick(&s_state, sentinel_now_ms(), drive_row);
    sentinel_bandhost_tick(&s_band, &s_state, sentinel_now_ms(), &s_sink);
    if (out.note[0]) sentinel_push_event(&s_state, out.note_warn, "deck", out.note, sentinel_now_ms(), &s_sink);
    if (out.set_baud) printf("[sim] sentinel baud %u\n", (unsigned)out.set_baud);
    if (up < 2) return; /* the C5 is booting: commands sent now are lost */
    for (int i = 0; i < out.n_send; i++) sentinel_send(out.send[i]);
    node_arm_tick();
    if (s_tick == 0) emit("HELLO,0.7.0,2,cc=1,nrf=1,lora=0,gps=1,wifi=1,ble=1,mon=1,drv=1,sub=1");
    if (s_node_tool != NODE_WIFI && s_node_tool != NODE_HPOT) ble_tick(); /* those two take the whole radio */
    wifi_tick();
    if (s_node_dwell_khz) dwell_tick(); /* the CC1101 listens to one frequency: no sweep */
    else emit_sub();
    if (s_tick % 8 == 7) emit_nrf();
    if (s_tick % 40 == 20) { /* each band's state again every ten seconds, normal or not */
        char row[40];
        if (sentinel_proto_bnd(row, sizeof(row), true, s_node_nrf.state, s_node_nrf.channels, 0)) emit(row);
        if (sentinel_proto_bnd(row, sizeof(row), false, s_node_sub.state, s_node_sub.channels, s_node_sub.delta_db)) emit(row);
    }
    if (s_tick % 4 == 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "STAT,%u,1,1,0,1,%u", (unsigned)up, 212000u - (unsigned)rnd(3000));
        emit(buf);
        if (up < 8) {
            emit("GPS,0,0,6,99.9,0.000000,0.000000,0.0,0.0,0,0,0");
        } else {
            /* On a wardrive the deck goes north up the street at 12 m/s. */
            if (s_node_tool == NODE_DRIVE) s_street_m += 12.0;
            snprintf(buf, sizeof(buf), "GPS,3,%d,12,0.9,%.6f,-73.989308,17.2,%.1f,45,%02u%02u%02u.00,121126", 8 + (int)rnd(3),
                     40.741895 + s_street_m / 111195.0, 36.0 + rnd(50) / 10.0, 14u + (unsigned)(up / 3600) % 10,
                     (unsigned)(up / 60) % 60, (unsigned)up % 60);
            emit(buf);
        }
    }
    if (s_tick % 40 == 8) emit_wifi();
    if (s_tick == 12) emit("EVT,info,gps,3D fix, 9 satellites");
    s_tick++;
}

void sentinel_start(void)
{
    if (s_on) return;
    sentinel_state_init(&s_state);
    s_lines.len     = 0;
    s_state.powered = true;
    s_state.gdo0 = false;
    s_state.gdo2 = true;
    s_state.nrf_irq = true;
    s_on      = true;
    s_tick    = 0;
    s_quiet   = false;
    s_started = lv_tick_get();
    sentinel_host_init(&s_host, SENTINEL_BAUD_FAST);
    sentinel_arm_set(&s_node_arm, false, 0, 0);
    sentinel_bleagg_init(&s_node_ble);
    sentinel_blehost_init(&s_ble);
    sentinel_wifimon_init(&s_node_wifi);
    sentinel_wifihost_init(&s_wifi);
    sentinel_bandhost_init(&s_band, sentinel_now_ms());
    sentinel_subwatch_init(&s_node_sub);
    sentinel_nrfwatch_init(&s_node_nrf);
    s_node_dwell_khz = 0;
    s_jam_at         = getenv("SIM_RF_JAM") ? atoi(getenv("SIM_RF_JAM")) : -1;
    s_node_tool     = NODE_WATCH;
    s_street_m      = 0;
    s_hop           = 0;
    s_hp_ssid[0]    = 0;
    s_node_ble_mode = s_node_cap = false;
    s_timer   = lv_timer_create(tick, 250, NULL);
    printf("[sim] sentinel on\n");
}

void sentinel_stop(void)
{
    if (!s_on) return;
    sentinel_drive_stop();
    s_on = false;
    s_state.powered = false;
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    sentinel_arm_set(&s_state.arm, false, 0, 0);
    sentinel_arm_set(&s_node_arm, false, 0, 0);
    node_arm_tick(); /* power is gone, and with it the lease */
    sentinel_store_close();
    printf("[sim] sentinel off\n");
}

bool sentinel_running(void) { return s_on; }

void sentinel_snapshot(sentinel_state_t *out)
{
    if (!s_on && !s_state.frames) sentinel_state_init(&s_state);
    memcpy(out, &s_state, sizeof(*out));
}

void sentinel_arm(bool on) { sentinel_host_arm(&s_host, on); }

void sentinel_note(bool warn, const char *text)
{
    if (s_on) sentinel_push_event(&s_state, warn, "deck", text, sentinel_now_ms(), &s_sink);
}

void sentinel_stats(sentinel_store_stats_t *out) { sentinel_store_stats(out); }

/* The two swept bands, as on the device. */
void sentinel_band_copy(sentinel_bandhost_t *out) { memcpy(out, &s_band, sizeof(*out)); }
void sentinel_band_learn(void) { sentinel_host_learn(&s_host); }
void sentinel_band_listen(uint32_t khz)
{
    uint32_t was = s_host.want_dwell_khz;
    sentinel_host_sub(&s_host, khz);
    if (s_host.want_dwell_khz && s_host.want_dwell_khz != was) sentinel_bursthost_init(&s_band.bursts);
}
void sentinel_site_begin(void) { sentinel_bandhost_stats_reset(&s_band, sentinel_now_ms()); }
bool sentinel_site_write(char *name, size_t n)
{
    memcpy(&s_band_site, &s_band, sizeof(s_band_site));
    if (!sentinel_store_site_from(&s_band_site, &s_state, sentinel_now_ms(), name, n)) return false;
    char note[64];
    snprintf(note, sizeof(note), "site survey written: %.31s", name);
    sentinel_note(false, note);
    return true;
}

/* Wardriving, as on the device but with no writer task in between. */
void sentinel_drive_start(const char *app_version)
{
    if (!s_on || sentinel_drivelog_on()) return;
    sentinel_store_stats_t st;
    sentinel_store_stats(&st);
    s_drive_lost_base = st.drive_dropped;
    sentinel_host_mode(&s_host, "drive");
    sentinel_wigle_id_t id = {.app = app_version, .model = "Simulator", .release = s_state.fw, .board = "host", .brand = "DECK//OS"};
    char                note[64];
    if (sentinel_store_drive_open(&id)) {
        sentinel_store_stats(&st);
        snprintf(note, sizeof(note), "wardrive started: %s", st.drive_file);
        sentinel_note(false, note);
    } else {
        sentinel_note(true, "wardrive: no file on the SD card, nothing is written");
    }
    sentinel_drivelog_begin(sentinel_now_ms());
}

void sentinel_drive_stop(void)
{
    if (!sentinel_drivelog_on()) return;
    sentinel_drivelog_end();
    sentinel_host_mode(&s_host, "watch");
    sentinel_store_drive_close();
    sentinel_drive_info_t info;
    char                  note[64];
    sentinel_drive_info(&info);
    snprintf(note, sizeof(note), "wardrive ended: %u rows, %u wifi, %u ble", (unsigned)info.rows, (unsigned)info.wifi,
             (unsigned)info.ble);
    sentinel_note(false, note);
}

void sentinel_drive_info(sentinel_drive_info_t *out)
{
    sentinel_drivelog_info(out);
    sentinel_store_stats_t st;
    sentinel_store_stats(&st);
    out->writing = st.drive_open;
    out->lost    = st.drive_dropped - s_drive_lost_base;
    memcpy(out->file, st.drive_file, sizeof(out->file));
}

void sentinel_tool(const char *mode) { sentinel_host_mode(&s_host, mode); }

void sentinel_wifi_copy(sentinel_wifihost_t *out)
{
    sentinel_wifihost_expire(&s_wifi, sentinel_now_ms());
    memcpy(out, &s_wifi, sizeof(*out));
    sentinel_wifihost_sort(out);
}

void sentinel_honeypot(uint8_t channel, const char *ssid)
{
    sentinel_host_honeypot(&s_host, channel, ssid);
    sentinel_host_mode(&s_host, "hpot");
}
void sentinel_capture(bool on) { sentinel_host_capture(&s_host, on); }

void sentinel_ble_table(sentinel_bletab_t *out)
{
    sentinel_bletab_expire(&s_ble.tab, sentinel_now_ms());
    memcpy(out, &s_ble.tab, sizeof(*out));
    sentinel_bletab_sort(out);
}

bool sentinel_ble_track(const uint8_t addr[6], sentinel_track_t *out)
{
    const sentinel_track_t *t = sentinel_trackwatch_find(&s_ble.watch, addr);
    if (t) *out = *t;
    return t != NULL;
}

bool sentinel_ble_mine(const uint8_t addr[6]) { return sentinel_trackwatch_mine(&s_ble.watch, addr); }
void sentinel_ble_set_mine(const uint8_t addr[6], bool mine) { sentinel_trackwatch_set_mine(&s_ble.watch, addr, mine); }

/* What the node does with a command (rf-sentinel main/main.cpp, handle_cmd). */
bool sentinel_send(const char *payload)
{
    if (!s_on) return false;
    if ((lv_tick_get() - s_started) / 1000 < 2) return true; /* still booting: sent, not heard */
    sentinel_cmd_t c   = sentinel_cmd_parse(payload);
    uint32_t       now = sentinel_now_ms();
    char           buf[64];
    switch (c.id) {
        case SENTINEL_CMD_HELLO: emit("HELLO,0.7.0,2,cc=1,nrf=1,lora=0,gps=1,wifi=1,ble=1,mon=1,drv=1,sub=1"); break;
        case SENTINEL_CMD_PING: emit("PONG,1234"); break;
        case SENTINEL_CMD_QUIET: s_quiet = true; break;
        case SENTINEL_CMD_STREAM: s_quiet = false; break;
        case SENTINEL_CMD_BAUD:
            if (!c.valid) {
                emit("ERR,bad baud");
                break;
            }
            snprintf(buf, sizeof(buf), "BAUD,%ld", c.value);
            emit(buf);
            break;
        case SENTINEL_CMD_MODE: {
            int tool = !c.valid ? -1 : !strcmp(c.arg, "watch") ? NODE_WATCH : !strcmp(c.arg, "ble") ? NODE_BLE
                       : !strcmp(c.arg, "wifi") ? NODE_WIFI : !strcmp(c.arg, "hpot") ? NODE_HPOT
                       : !strcmp(c.arg, "drive") ? NODE_DRIVE : -1;
            if (tool < 0) {
                emit("ERR,unknown mode");
            } else if (tool == NODE_HPOT && !sentinel_arm_active(&s_node_arm, now)) {
                emit("ERR,not armed"); /* the honeypot transmits: the lease is the gate */
            } else if (tool == NODE_HPOT && !s_hp_ssid[0]) {
                emit("ERR,honeypot not set up");
            } else {
                if (tool != (int)s_node_tool) {
                    node_capture(false); /* capture belongs to the tool that asked for it */
                    printf("[sim] sentinel mode %s\n", c.arg);
                    if (tool == NODE_WIFI || tool == NODE_DRIVE) sentinel_wifimon_dwell(&s_node_wifi, sentinel_wifi_channel[s_hop = 0], 0, NULL);
                    if (tool == NODE_HPOT) s_hp_tick = s_hp_joins = s_hp_probes = 0;
                }
                s_node_tool     = tool;
                s_node_ble_mode = tool == NODE_BLE;
                snprintf(buf, sizeof(buf), "MODE,%s", c.arg);
                emit(buf);
            }
            break;
        }
        case SENTINEL_CMD_LEARN:
            sentinel_subwatch_init(&s_node_sub);
            sentinel_nrfwatch_init(&s_node_nrf);
            emit("BND,sub,0,0,0");
            emit("BND,nrf,0,0,0");
            emit("EVT,info,node,learning the bands again");
            break;
        case SENTINEL_CMD_SUB:
            if (!c.valid) {
                emit("ERR,bad sub");
                break;
            }
            if (c.value && sentinel_subwatch_pause(&s_node_sub)) emit("BND,sub,0,0,0");
            if ((uint32_t)c.value != s_node_dwell_khz) {
                printf("[sim] sentinel 433 MHz %s %ld\n", c.value ? "listening on" : "sweeping", c.value);
                s_node_dwell_khz = (uint32_t)c.value;
                s_dwell_tick = s_dwell_bursts = 0;
            }
            snprintf(buf, sizeof(buf), "SBD,%lu,%d,0", (unsigned long)s_node_dwell_khz, s_node_dwell_khz ? -104 : 0);
            emit(buf);
            break;
        case SENTINEL_CMD_HPOT:
            if (!c.valid) {
                emit("ERR,bad honeypot");
                break;
            }
            s_hp_ch = (uint8_t)c.value;
            snprintf(s_hp_ssid, sizeof(s_hp_ssid), "%s", c.arg);
            snprintf(buf, sizeof(buf), "HPT,%u,0,0,0,", (unsigned)s_hp_ch);
            {
                char row[96];
                snprintf(row, sizeof(row), "%s%s", buf, s_hp_ssid);
                emit(row);
            }
            break;
        case SENTINEL_CMD_CAP:
            if (!c.valid) emit("ERR,bad cap");
            else if (c.value == 0) s_node_cap ? node_capture(false) : emit("CAP,0");
            else if (s_node_tool != NODE_BLE && s_node_tool != NODE_WIFI) emit("ERR,capture needs mode ble or wifi");
            else s_node_cap ? emit("CAP,1") : node_capture(true);
            break;
        case SENTINEL_CMD_ARM: {
            if (!c.valid) {
                emit("ERR,bad arm");
                break;
            }
            bool was = sentinel_arm_active(&s_node_arm, now);
            sentinel_arm_set(&s_node_arm, c.value == 1, now, SENTINEL_ARM_LEASE_MS);
            snprintf(buf, sizeof(buf), "ARM,%ld,%u", c.value, (unsigned)sentinel_arm_left_ms(&s_node_arm, now));
            emit(buf);
            if (c.value == 1 && !was) emit("EVT,info,node,armed");
            if (c.value == 0 && was) emit("EVT,info,node,disarmed");
            node_arm_tick();
            break;
        }
        case SENTINEL_CMD_TX: emit(sentinel_arm_active(&s_node_arm, now) ? "ERR,no such test" : "ERR,not armed"); break;
        default: emit("ERR,unknown command"); break;
    }
    return true;
}
