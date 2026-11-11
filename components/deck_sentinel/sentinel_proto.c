/*
 * $RFS sentence parser. See sentinel_proto.h for the frame layout and
 * rf-sentinel/main/main.cpp for what the C5 puts in each one.
 */
#include "sentinel_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELDS_MAX 32 /* SUB has 23 fields; everything else fewer */

static uint8_t xor_sum(const char *p, size_t n)
{
    uint8_t c = 0;
    while (n--) c ^= (uint8_t)*p++;
    return c;
}

static void copy_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src);
}

/* Split on commas in place. Returns the field count. */
static int split(char *s, char *f[], int max)
{
    int n = 0;
    while (n < max && s) {
        f[n++] = s;
        s = strchr(s, ',');
        if (s) *s++ = 0;
    }
    return n;
}

/* Fields from..n-1 back into one string: text that may itself hold commas. */
static void rejoin(char *dst, size_t n, char *f[], int from, int count)
{
    dst[0] = 0;
    for (int i = from; i < count; i++) {
        size_t len = strlen(dst);
        snprintf(dst + len, n - len, "%s%s", i > from ? "," : "", f[i]);
    }
}

static void push_event(sentinel_state_t *s, bool warn, const char *src, const char *text, uint32_t now_ms)
{
    sentinel_event_t *e = &s->evt[s->evt_head];
    e->ms               = now_ms;
    e->warn             = warn;
    copy_str(e->src, sizeof(e->src), src);
    copy_str(e->text, sizeof(e->text), text);
    s->evt_head = (uint8_t)((s->evt_head + 1) % SENTINEL_EVT_MAX);
    if (s->evt_count < SENTINEL_EVT_MAX) s->evt_count++;
    if (warn) s->warn_count++;
}

/* "cc=1" style flags in HELLO; the order is fixed today but names are safer. */
static void hello_flag(sentinel_state_t *s, const char *kv)
{
    const char *eq = strchr(kv, '=');
    if (!eq) return;
    bool on = eq[1] == '1';
    if (!strncmp(kv, "cc=", 3)) s->cc_ok = on;
    else if (!strncmp(kv, "nrf=", 4)) s->nrf_ok = on;
    else if (!strncmp(kv, "lora=", 5)) s->lora_ok = on;
    else if (!strncmp(kv, "gps=", 4)) s->gps_ok = on;
    else if (!strncmp(kv, "wifi=", 5)) s->wifi_ok = on;
}

void sentinel_state_init(sentinel_state_t *s)
{
    memset(s, 0, sizeof(*s));
    copy_str(s->fw, sizeof(s->fw), "?");
    copy_str(s->proto, sizeof(s->proto), "?");
    copy_str(s->utc, sizeof(s->utc), "0");
    s->wifi_strongest = -127;
    for (int i = 0; i < SENTINEL_SUB_CH; i++) s->sub[i] = s->sub_peak[i] = SENTINEL_SUB_FLOOR_DBM;
}

bool sentinel_proto_line(sentinel_state_t *s, char *line, uint32_t now_ms)
{
    /* Frame check: "$RFS," prefix and a matching checksum after '*'. */
    char *star = line[0] == '$' ? strchr(line, '*') : NULL;
    if (!star) {
        s->bad++;
        return false;
    }
    unsigned want;
    if (sscanf(star + 1, "%2x", &want) != 1 || xor_sum(line + 1, (size_t)(star - line - 1)) != (uint8_t)want ||
        strncmp(line, "$RFS,", 5) != 0) {
        s->bad++;
        return false;
    }
    *star = 0;

    char *f[FIELDS_MAX];
    int   n = split(line + 5, f, FIELDS_MAX);
    if (n < 1) {
        s->bad++;
        return false;
    }
    s->frames++;
    s->last_ms  = now_ms;
    const char *t = f[0];

    if (!strcmp(t, "HELLO") && n >= 3) {
        copy_str(s->fw, sizeof(s->fw), f[1]);
        copy_str(s->proto, sizeof(s->proto), f[2]);
        for (int i = 3; i < n; i++) hello_flag(s, f[i]);
        s->hello = true;
        push_event(s, false, "node", "node online", now_ms);
    } else if (!strcmp(t, "STAT") && n >= 7) {
        s->uptime_s = (uint32_t)strtoul(f[1], NULL, 10);
        s->cc_ok    = f[2][0] == '1';
        s->nrf_ok   = f[3][0] == '1';
        s->lora_ok  = f[4][0] == '1';
        s->gps_ok   = f[5][0] == '1';
        s->heap     = (uint32_t)strtoul(f[6], NULL, 10);
    } else if (!strcmp(t, "GPS") && n >= 11) {
        s->fix       = (uint8_t)atoi(f[1]);
        s->sats_used = (uint8_t)atoi(f[2]);
        s->sats_view = (uint8_t)atoi(f[3]);
        s->hdop      = (float)atof(f[4]);
        s->lat       = atof(f[5]);
        s->lon       = atof(f[6]);
        s->alt_m     = (float)atof(f[7]);
        s->cn0_avg   = (float)atof(f[8]);
        s->cn0_max   = (uint8_t)atoi(f[9]);
        copy_str(s->utc, sizeof(s->utc), f[10]);
    } else if (!strcmp(t, "SUB") && n >= 2) {
        int count = atoi(f[1]);
        if (count > SENTINEL_SUB_CH) count = SENTINEL_SUB_CH;
        if (n < 2 + count) count = n - 2;
        for (int i = 0; i < count; i++) {
            int v = atoi(f[2 + i]);
            if (v < -127) v = -127;
            if (v > 0) v = 0;
            s->sub[i] = (int8_t)v;
            /* The peak drifts down 1 dB per sweep, so a burst stays visible
             * for a few seconds after it ends. */
            int peak = s->sub_peak[i] - 1;
            if (peak < SENTINEL_SUB_FLOOR_DBM) peak = SENTINEL_SUB_FLOOR_DBM;
            s->sub_peak[i] = (int8_t)(v > peak ? v : peak);
        }
        s->sub_sweeps++;
    } else if (!strcmp(t, "NRF") && n >= 3) {
        const char *h = f[2];
        for (int i = 0; i < SENTINEL_NRF_CH && h[i]; i++) {
            char c = h[i];
            int  v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
            if (v > SENTINEL_NRF_MAX_HITS) v = SENTINEL_NRF_MAX_HITS;
            s->nrf[i]  = (uint8_t)v;
            int peak   = s->nrf_peak[i] ? s->nrf_peak[i] - 1 : 0;
            s->nrf_peak[i] = (uint8_t)(v > peak ? v : peak);
        }
        s->nrf_sweeps++;
    } else if (!strcmp(t, "LORA") && n >= 4) {
        s->lora_pkts += atoi(f[1]);
        s->lora_rssi = (float)atof(f[2]);
        s->lora_snr  = (float)atof(f[3]);
    } else if (!strcmp(t, "WIFI") && n >= 5) {
        s->wifi_total     = (uint16_t)atoi(f[1]);
        s->wifi_24        = (uint16_t)atoi(f[2]);
        s->wifi_5         = (uint16_t)atoi(f[3]);
        s->wifi_strongest = (int8_t)atoi(f[4]);
        s->ap_fill        = 0; /* the WAP rows that follow are a new list */
        s->wifi_scans++;
    } else if (!strcmp(t, "WAP") && n >= 5) {
        if (s->ap_fill < SENTINEL_AP_MAX) {
            sentinel_ap_t *a = &s->ap[s->ap_fill++];
            copy_str(a->bssid, sizeof(a->bssid), f[1]);
            a->ch   = (uint8_t)atoi(f[2]);
            a->rssi = (int8_t)atoi(f[3]);
            rejoin(a->ssid, sizeof(a->ssid), f, 4, n); /* an SSID may contain commas */
            s->ap_n = s->ap_fill;
        }
    } else if (!strcmp(t, "EVT") && n >= 4) {
        char text[64];
        rejoin(text, sizeof(text), f, 3, n);
        push_event(s, !strcmp(f[1], "warn"), f[2], text, now_ms);
    }
    /* PONG, ERR and anything newer: a valid frame, nothing to keep. */
    return true;
}

int sentinel_proto_feed(sentinel_lines_t *l, sentinel_state_t *s, const char *data, size_t n, uint32_t now_ms)
{
    int good = 0;
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            if (l->len) {
                l->buf[l->len] = 0;
                if (sentinel_proto_line(s, l->buf, now_ms)) good++;
                l->len = 0;
            }
        } else if (l->len < sizeof(l->buf) - 1) {
            l->buf[l->len++] = c;
        } else {
            l->len = 0; /* longer than any sentence: noise, drop it */
        }
    }
    return good;
}

int sentinel_proto_frame(char *out, size_t n, const char *payload)
{
    char body[SENTINEL_LINE_MAX];
    int  bl = snprintf(body, sizeof(body), "RFS,%s", payload);
    if (bl < 0 || bl >= (int)sizeof(body)) return 0;
    int len = snprintf(out, n, "$%s*%02X\n", body, xor_sum(body, (size_t)bl));
    return (len < 0 || len >= (int)n) ? 0 : len;
}

/* ---- Helpers shared by both platforms ----------------------------------- */

bool sentinel_link(const sentinel_state_t *s)
{
    return s->powered && s->frames && (uint32_t)(sentinel_now_ms() - s->last_ms) < SENTINEL_LINK_MS;
}

const sentinel_event_t *sentinel_event(const sentinel_state_t *s, int newest_index)
{
    if (newest_index < 0 || newest_index >= s->evt_count) return NULL;
    int i = (int)s->evt_head - 1 - newest_index;
    while (i < 0) i += SENTINEL_EVT_MAX;
    return &s->evt[i];
}
