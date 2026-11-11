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
 */
#include "deck_sentinel.h"
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
static const sentinel_sink_t s_sink = {sink_event, sink_packet, NULL};

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
    if (s_tick == 0) emit("HELLO,0.3.0,2,cc=1,nrf=1,lora=0,gps=1,wifi=1");
    if (s_tick % 20 == 15) emit_pkt();
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

/* What the node does with a command (rf-sentinel main/main.cpp, handle_cmd). */
bool sentinel_send(const char *payload)
{
    if (!s_on) return false;
    if ((lv_tick_get() - s_started) / 1000 < 2) return true; /* still booting: sent, not heard */
    sentinel_cmd_t c   = sentinel_cmd_parse(payload);
    uint32_t       now = sentinel_now_ms();
    char           buf[64];
    switch (c.id) {
        case SENTINEL_CMD_HELLO: emit("HELLO,0.3.0,2,cc=1,nrf=1,lora=0,gps=1,wifi=1"); break;
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
        case SENTINEL_CMD_MODE: emit(c.valid && !strcmp(c.arg, "watch") ? "MODE,watch" : "ERR,unknown mode"); break;
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
