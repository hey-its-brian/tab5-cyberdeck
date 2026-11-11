/*
 * The $RFS sentence parser, with no hardware behind it so the simulator and
 * the host unit test run the same code as the device.
 *
 *   $RFS,<TYPE>,<field>,...*<XOR of everything between $ and *, 2 hex>\n
 *
 * Sentences: HELLO (fw, proto, cc=, nrf=, lora=, gps=, wifi=), STAT (uptime,
 * cc, nrf, lora, gps, heap), GPS (fix, used, view, hdop, lat, lon, alt, C/N0
 * avg, C/N0 max, utc), SUB (n, n x dBm), NRF (n, n hex digits), LORA (pkts,
 * rssi, snr), WIFI (total, 2.4, 5, strongest), WAP (bssid, ch, rssi, ssid),
 * EVT (warn|info, source, text), PONG, ERR.
 */
#pragma once

#include "deck_sentinel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENTINEL_LINE_MAX 320 /* longest sentence is NRF at about 150 bytes */

/* Line assembly for a byte stream. */
typedef struct {
    char   buf[SENTINEL_LINE_MAX];
    size_t len;
} sentinel_lines_t;

/* Floors and empties: what the state looks like before the first frame. */
void sentinel_state_init(sentinel_state_t *s);

/* Apply one line (no newline; the buffer is modified). Returns true for a
 * valid $RFS frame, which also bumps `frames` and `last_ms`; anything else
 * bumps `bad`. */
bool sentinel_proto_line(sentinel_state_t *s, char *line, uint32_t now_ms);

/* Feed raw bytes; every complete line goes through sentinel_proto_line().
 * Returns how many valid frames were in them. */
int sentinel_proto_feed(sentinel_lines_t *l, sentinel_state_t *s, const char *data, size_t n, uint32_t now_ms);

/* Write "$RFS,<payload>*CS\n" into out. Returns its length, 0 if it does not fit. */
int sentinel_proto_frame(char *out, size_t n, const char *payload);

#ifdef __cplusplus
}
#endif
