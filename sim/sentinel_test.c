/* Unit test for the $RFS parser (components/deck_sentinel/sentinel_proto.c). */
#include <stdio.h>
#include <string.h>

#include "sentinel_proto.h"

static int s_fail;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);         \
            s_fail++;                                                     \
        }                                                                 \
    } while (0)

static uint32_t s_now = 1000;
uint32_t sentinel_now_ms(void) { return s_now; }

/* Feed one payload as a well-formed frame. */
static bool send(sentinel_state_t *s, sentinel_lines_t *l, const char *payload)
{
    char frame[SENTINEL_LINE_MAX + 8];
    int  n = sentinel_proto_frame(frame, sizeof(frame), payload);
    return n > 0 && sentinel_proto_feed(l, s, frame, (size_t)n, s_now) == 1;
}

int main(void)
{
    sentinel_state_t s;
    sentinel_lines_t l = {0};
    sentinel_state_init(&s);
    s.powered = true;

    /* Framing: the checksum the C5 computes, a bad one, a foreign sentence. */
    char frame[64];
    int  n = sentinel_proto_frame(frame, sizeof(frame), "CMD,PING");
    CHECK(n > 0 && strcmp(frame, "$RFS,CMD,PING*1D\n") == 0);
    char bad[] = "$RFS,STAT,1,1,1,0,1,100*00";
    CHECK(!sentinel_proto_line(&s, bad, s_now) && s.bad == 1);
    char nmea[] = "$GPGGA,123519,4807.038,N*47";
    CHECK(!sentinel_proto_line(&s, nmea, s_now) && s.bad == 2);
    char nostar[] = "$RFS,STAT,1,1,1,0,1,100";
    CHECK(!sentinel_proto_line(&s, nostar, s_now) && s.bad == 3);
    CHECK(s.frames == 0 && !sentinel_link(&s));

    /* A stream split at odd places, with CRLF, still yields whole frames. */
    const char *stream = "$RFS,HELLO,0.1.0,1,cc=1,nrf=0,lora=0,gps=1,wifi=1*";
    char        want[8];
    {
        /* compute the checksum the same way the firmware does */
        uint8_t     c = 0;
        const char *p = stream + 1;
        while (*p != '*') c ^= (uint8_t)*p++;
        snprintf(want, sizeof(want), "%02X\r\n", c);
    }
    CHECK(sentinel_proto_feed(&l, &s, stream, strlen(stream), s_now) == 0);
    CHECK(sentinel_proto_feed(&l, &s, want, strlen(want), s_now) == 1);
    CHECK(s.hello && s.frames == 1 && strcmp(s.fw, "0.1.0") == 0 && strcmp(s.proto, "1") == 0);
    CHECK(s.cc_ok && !s.nrf_ok && !s.lora_ok && s.gps_ok && s.wifi_ok);
    CHECK(s.evt_count == 1 && strcmp(sentinel_event(&s, 0)->text, "node online") == 0);
    CHECK(sentinel_link(&s));

    CHECK(send(&s, &l, "STAT,3600,1,1,0,1,181234"));
    CHECK(s.uptime_s == 3600 && s.nrf_ok && s.heap == 181234);

    CHECK(send(&s, &l, "GPS,3,9,12,0.9,40.741895,-73.989308,17.2,38.5,45,153042.00"));
    CHECK(s.fix == 3 && s.sats_used == 9 && s.sats_view == 12 && s.cn0_max == 45);
    CHECK(s.lat > 40.74 && s.lat < 40.75 && s.lon < -73.98 && s.lon > -73.99);
    CHECK(strcmp(s.utc, "153042.00") == 0);

    /* SUB: 21 channels, peaks hold then decay 1 dB per sweep. */
    char sub[SENTINEL_LINE_MAX];
    int  len = snprintf(sub, sizeof(sub), "SUB,%d", SENTINEL_SUB_CH);
    for (int i = 0; i < SENTINEL_SUB_CH; i++) len += snprintf(sub + len, sizeof(sub) - len, ",%d", i == 9 ? -60 : -110);
    CHECK(send(&s, &l, sub));
    CHECK(s.sub[9] == -60 && s.sub[0] == -110 && s.sub_peak[9] == -60 && s.sub_sweeps == 1);
    len = snprintf(sub, sizeof(sub), "SUB,%d", SENTINEL_SUB_CH);
    for (int i = 0; i < SENTINEL_SUB_CH; i++) len += snprintf(sub + len, sizeof(sub) - len, ",%d", -110);
    CHECK(send(&s, &l, sub));
    CHECK(s.sub[9] == -110 && s.sub_peak[9] == -61);
    /* A short SUB (fewer values than announced) does not read past the fields. */
    CHECK(send(&s, &l, "SUB,21,-100,-99"));
    CHECK(s.sub[0] == -100 && s.sub[1] == -99 && s.sub[2] == -110);

    /* NRF: one hex digit per channel. */
    char nrf[SENTINEL_LINE_MAX];
    len = snprintf(nrf, sizeof(nrf), "NRF,%d,", SENTINEL_NRF_CH);
    for (int i = 0; i < SENTINEL_NRF_CH; i++) nrf[len++] = i == 62 ? '8' : i == 80 ? 'A' : '0';
    nrf[len] = 0;
    CHECK(send(&s, &l, nrf));
    CHECK(s.nrf[62] == 8 && s.nrf[80] == 8 && s.nrf[0] == 0 && s.nrf_peak[62] == 8 && s.nrf_sweeps == 1);

    /* WIFI starts a new list; WAP rows fill it; an SSID may hold commas. */
    CHECK(send(&s, &l, "WIFI,23,14,9,-41"));
    CHECK(s.wifi_total == 23 && s.wifi_24 == 14 && s.wifi_5 == 9 && s.wifi_strongest == -41 && s.ap_fill == 0);
    CHECK(send(&s, &l, "WAP,A4C3F0112233,1,-41,NIGHTCITY-5G"));
    CHECK(send(&s, &l, "WAP,F0B4D277889A,11,-84,Kiroshi, Optics"));
    CHECK(s.ap_n == 2 && strcmp(s.ap[1].ssid, "Kiroshi, Optics") == 0 && s.ap[1].ch == 11 && s.ap[1].rssi == -84);
    CHECK(strcmp(s.ap[0].bssid, "A4C3F0112233") == 0);
    for (int i = 0; i < 10; i++) CHECK(send(&s, &l, "WAP,000000000000,6,-90,x"));
    CHECK(s.ap_n == SENTINEL_AP_MAX);
    CHECK(send(&s, &l, "WIFI,1,1,0,-50"));
    CHECK(s.ap_fill == 0 && s.ap_n == SENTINEL_AP_MAX); /* the old list shows until rows replace it */

    /* EVT text is rejoined; warn is counted; the ring keeps the newest. */
    CHECK(send(&s, &l, "EVT,warn,sub,433.9 MHz active (-61 dBm)"));
    CHECK(s.warn_count == 1 && sentinel_event(&s, 0)->warn && strcmp(sentinel_event(&s, 0)->src, "sub") == 0);
    CHECK(send(&s, &l, "EVT,info,gps,3D fix, 9 satellites"));
    CHECK(strcmp(sentinel_event(&s, 0)->text, "3D fix, 9 satellites") == 0 && !sentinel_event(&s, 0)->warn);
    CHECK(strcmp(sentinel_event(&s, 1)->text, "433.9 MHz active (-61 dBm)") == 0);
    for (int i = 0; i < 10; i++) CHECK(send(&s, &l, "EVT,info,nrf,2462 MHz quiet"));
    CHECK(s.evt_count == SENTINEL_EVT_MAX && sentinel_event(&s, SENTINEL_EVT_MAX) == NULL);

    /* Unknown but valid frames count as frames. */
    CHECK(send(&s, &l, "PONG,1234") && send(&s, &l, "ERR,unknown command"));

    /* Link drops after the timeout. */
    s_now += SENTINEL_LINK_MS + 1;
    CHECK(!sentinel_link(&s));

    /* Noise: a line longer than any sentence is dropped without harm. */
    char junk[SENTINEL_LINE_MAX * 2];
    memset(junk, 'x', sizeof(junk));
    junk[sizeof(junk) - 1] = '\n';
    uint32_t bad_before = s.bad;
    CHECK(sentinel_proto_feed(&l, &s, junk, sizeof(junk), s_now) == 0);
    CHECK(s.bad >= bad_before);
    CHECK(send(&s, &l, "PONG,1")); /* and the next frame still parses */

    printf("%s (%d failures)\n", s_fail ? "FAIL" : "PASS", s_fail);
    return s_fail ? 1 : 0;
}
