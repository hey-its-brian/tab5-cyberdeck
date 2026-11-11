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
 */
#include "deck_sentinel.h"
#include "sentinel_bleagg.h"
#include "sentinel_cmd.h"
#include "sentinel_host.h"
#include "sentinel_proto.h"

#include <stdio.h>
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

uint32_t sentinel_now_ms(void) { return lv_tick_get(); }

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
static void sink_device(void *user, const sentinel_bdv_t *d)
{
    (void)user;
    sentinel_blehost_device(&s_ble, &s_state, d, sentinel_now_ms(), &s_sink);
}
static const sentinel_sink_t s_sink = {sink_event, sink_packet, sink_device, NULL};

static void emit(const char *payload)
{
    char frame[SENTINEL_LINE_MAX + 8];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    if (n > 0) sentinel_proto_feed(&s_lines, &s_state, frame, (size_t)n, sentinel_now_ms(), &s_sink);
}

/* A beacon from NIGHTCITY-5G, as the C5's monitor mode would hand it over. */
static void emit_pkt(void)
{
    static const uint8_t beacon[] = {
        0x80, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             /* beacon, to everyone */
        0xa4, 0xc3, 0xf0, 0x11, 0x22, 0x33, 0xa4, 0xc3, 0xf0, 0x11, 0x22, 0x33, /* from and BSSID */
        0x10, 0x00,                                                             /* sequence */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x11, 0x04, /* timestamp, interval, capabilities */
        0x00, 0x0c, 'N',  'I',  'G',  'H',  'T',  'C',  'I',  'T',  'Y',  '-',  '5', 'G', /* SSID */
        0x03, 0x01, 0x01,                                                       /* channel 1 */
    };
    sentinel_pkt_t p = {.link = SENTINEL_LINK_WIFI, .ch = 1, .rssi = -41, .us = (lv_tick_get() - s_started) * 1000u};
    p.len = p.orig_len = sizeof(beacon);
    memcpy(p.data, beacon, sizeof(beacon));
    char payload[SENTINEL_LINE_MAX];
    if (sentinel_proto_pkt(payload, sizeof(payload), &p)) emit(payload);
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
    {{0x4D, 0x39, 0xB7, 0x05, 0xEE, 0x21}, false, -60, 0, APPLE(6, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x24, 0x02), 10},
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
        int8_t rssi = (int8_t)(d->rssi - 3 + (int)rnd(7));
        sentinel_bleagg_feed(&s_node_ble, d->addr, d->pub, rssi, d->adv, d->len, now);
        if (s_node_cap) { /* the same advertisement, as a captured packet */
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
}

static void emit_sub(void)
{
    char buf[SENTINEL_LINE_MAX];
    int  n = snprintf(buf, sizeof(buf), "SUB,%d", SENTINEL_SUB_CH);
    bool fob = (s_tick % 40) < 6; /* 1.5 s chirp every 10 s */
    for (int i = 0; i < SENTINEL_SUB_CH; i++) {
        int v = -112 + (int)rnd(9);
        if (i == 9 && fob) v = -58 - (int)rnd(6);           /* 433.9 MHz */
        if ((i == 8 || i == 10) && fob) v = -80 - (int)rnd(8); /* its shoulders */
        n += snprintf(buf + n, sizeof(buf) - n, ",%d", v);
    }
    emit(buf);
}

static void emit_nrf(void)
{
    char buf[SENTINEL_LINE_MAX];
    int  n = snprintf(buf, sizeof(buf), "NRF,%d,", SENTINEL_NRF_CH);
    for (int i = 0; i < SENTINEL_NRF_CH; i++) {
        int mhz = SENTINEL_NRF_FIRST_MHZ + i, hits = rnd(10) == 0 ? 1 : 0;
        if (mhz >= 2402 && mhz <= 2422) hits = 3 + (int)rnd(5); /* channel 1 */
        if (mhz >= 2427 && mhz <= 2447) hits = 2 + (int)rnd(4); /* channel 6 */
        if (mhz >= 2452 && mhz <= 2472) hits = 5 + (int)rnd(4); /* channel 11 */
        if (mhz == 2480) hits = 1 + (int)rnd(3);                /* a BLE advertiser */
        buf[n++] = "0123456789ABCDEF"[hits & 0xF];
    }
    buf[n] = 0;
    emit(buf);
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
    if (out.note[0]) sentinel_push_event(&s_state, out.note_warn, "deck", out.note, sentinel_now_ms(), &s_sink);
    if (out.set_baud) printf("[sim] sentinel baud %u\n", (unsigned)out.set_baud);
    if (up < 2) return; /* the C5 is booting: commands sent now are lost */
    for (int i = 0; i < out.n_send; i++) sentinel_send(out.send[i]);
    node_arm_tick();
    if (s_tick == 0) emit("HELLO,0.4.0,2,cc=1,nrf=1,lora=0,gps=1,wifi=1,ble=1");
    if (s_tick % 20 == 15) emit_pkt();
    ble_tick();
    if (!s_quiet) {
        emit_sub();
        if (s_tick % 8 == 7) emit_nrf();
    }
    if (s_tick % 4 == 0) {
        char buf[160];
        snprintf(buf, sizeof(buf), "STAT,%u,1,1,0,1,%u", (unsigned)up, 212000u - (unsigned)rnd(3000));
        emit(buf);
        if (up < 8) {
            emit("GPS,0,0,6,99.9,0.000000,0.000000,0.0,0.0,0,0");
        } else {
            snprintf(buf, sizeof(buf), "GPS,3,%d,12,0.9,40.741895,-73.989308,17.2,%.1f,45,%02u%02u%02u", 8 + (int)rnd(3),
                     36.0 + rnd(50) / 10.0, 14u + (unsigned)(up / 3600) % 10, (unsigned)(up / 60) % 60, (unsigned)up % 60);
            emit(buf);
        }
    }
    if (s_tick % 40 == 8) emit_wifi();
    if (s_tick == 12) emit("EVT,info,gps,3D fix, 9 satellites");
    if (s_tick % 40 == 2) emit("EVT,warn,sub,433.9 MHz active (-61 dBm)");
    if (s_tick % 40 == 9) emit("EVT,info,sub,433.9 MHz quiet");
    if (s_tick == 60) emit("EVT,warn,nrf,2462 MHz busy (8/8)");
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
    s_node_ble_mode = s_node_cap = false;
    s_timer   = lv_timer_create(tick, 250, NULL);
    printf("[sim] sentinel on\n");
}

void sentinel_stop(void)
{
    if (!s_on) return;
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

void sentinel_tool(const char *mode) { sentinel_host_mode(&s_host, mode); }
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
        case SENTINEL_CMD_HELLO: emit("HELLO,0.4.0,2,cc=1,nrf=1,lora=0,gps=1,wifi=1,ble=1"); break;
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
        case SENTINEL_CMD_MODE:
            if (c.valid && (!strcmp(c.arg, "watch") || !strcmp(c.arg, "ble"))) {
                bool ble = c.arg[0] == 'b';
                if (!ble) node_capture(false); /* capture belongs to the tool that asked for it */
                if (ble != s_node_ble_mode) printf("[sim] sentinel mode %s\n", c.arg);
                s_node_ble_mode = ble;
                emit(ble ? "MODE,ble" : "MODE,watch");
            } else {
                emit("ERR,unknown mode");
            }
            break;
        case SENTINEL_CMD_CAP:
            if (!c.valid) emit("ERR,bad cap");
            else if (c.value == 0) s_node_cap ? node_capture(false) : emit("CAP,0");
            else if (!s_node_ble_mode) emit("ERR,capture needs mode ble");
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
