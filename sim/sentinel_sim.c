/*
 * deck_sentinel for the simulator: a pretend Sentinel board. It composes
 * the same $RFS sentences the C5 sends and pushes them through the real
 * parser, so RF WATCH in the sim exercises everything but the UART.
 *
 * The scene: a quiet 433 MHz band with a key fob chirping on 433.9 MHz now
 * and then, 2.4 GHz busy where Wi-Fi channels 1, 6 and 11 sit, a GPS that
 * finds a 3D fix after a few seconds, and the networks net_sim.c invents.
 */
#include "deck_sentinel.h"
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

uint32_t sentinel_now_ms(void) { return lv_tick_get(); }

static uint32_t rnd(uint32_t n)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (s_rng >> 8) % n;
}

static void emit(const char *payload)
{
    char frame[SENTINEL_LINE_MAX + 8];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    if (n > 0) sentinel_proto_feed(&s_lines, &s_state, frame, (size_t)n, sentinel_now_ms());
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
    if (up < 2) return; /* the C5 is booting */
    if (s_tick == 0) emit("HELLO,0.1.0,1,cc=1,nrf=1,lora=0,gps=1,wifi=1");
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
    printf("[sim] sentinel off\n");
}

bool sentinel_running(void) { return s_on; }

void sentinel_snapshot(sentinel_state_t *out)
{
    if (!s_on && !s_state.frames) sentinel_state_init(&s_state);
    memcpy(out, &s_state, sizeof(*out));
}

bool sentinel_send(const char *payload)
{
    if (!s_on) return false;
    if (!strcmp(payload, "CMD,HELLO")) emit("HELLO,0.1.0,1,cc=1,nrf=1,lora=0,gps=1,wifi=1");
    else if (!strcmp(payload, "CMD,PING")) emit("PONG,1234");
    else if (!strcmp(payload, "CMD,QUIET")) s_quiet = true;
    else if (!strcmp(payload, "CMD,STREAM")) s_quiet = false;
    return true;
}
