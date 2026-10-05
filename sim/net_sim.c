/*
 * deck_net for the simulator: a pretend radio with a few networks, and real
 * HTTP through curl so the WEATHER module can fetch live data on the Mac.
 */
#include "deck_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_hal.h"
#include "lvgl.h"

static net_state_t s_state = NET_STARTING;
static char s_ssid[33];
static uint32_t s_connect_at;
static uint32_t s_scan_at;

static const net_ap_t s_fake[] = {
    {"NIGHTCITY-5G", -48, true}, {"ARASAKA-GUEST", -63, false}, {"Afterlife", -70, true},
    {"netwatch_honeypot", -82, true},
};

/* Simulated timing: the radio comes up 300 ms after boot, joins in 800 ms. */
static void tick(void)
{
    uint32_t now = lv_tick_get();
    if (s_state == NET_STARTING && now > 300) {
        hal_cfg_get_str("wifi_ssid", s_ssid, sizeof(s_ssid));
        s_state      = s_ssid[0] ? NET_CONNECTING : NET_IDLE;
        s_connect_at = now;
    }
    if (s_state == NET_CONNECTING && now - s_connect_at > 800) s_state = NET_CONNECTED;
}

void net_init(void) {}

net_state_t net_state(void)
{
    tick();
    return s_state;
}

const char *net_state_name(net_state_t s)
{
    switch (s) {
        case NET_STARTING: return "STARTING";
        case NET_NO_RADIO: return "NO RADIO";
        case NET_IDLE: return "NOT SET UP";
        case NET_CONNECTING: return "CONNECTING";
        case NET_CONNECTED: return "ONLINE";
    }
    return "?";
}

const char *net_ssid(void) { return s_ssid; }
const char *net_ip(void) { return net_state() == NET_CONNECTED ? "192.168.1.77" : ""; }
int net_rssi(void) { return net_state() == NET_CONNECTED ? -52 : 0; }
int net_bars(void) { return net_state() == NET_CONNECTED ? 4 : 0; }

void net_connect(const char *ssid, const char *pass)
{
    hal_cfg_set_str("wifi_ssid", ssid);
    hal_cfg_set_str("wifi_pass", pass);
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    s_state      = NET_CONNECTING;
    s_connect_at = lv_tick_get();
}

void net_forget(void)
{
    hal_cfg_set_str("wifi_ssid", NULL);
    s_ssid[0] = '\0';
    s_state   = NET_IDLE;
}

bool net_scan_start(void)
{
    s_scan_at = lv_tick_get();
    return true;
}

bool net_scan_done(void) { return lv_tick_elaps(s_scan_at) > 600; }

size_t net_scan_results(net_ap_t *out, size_t max)
{
    size_t n = sizeof(s_fake) / sizeof(s_fake[0]);
    if (n > max) n = max;
    memcpy(out, s_fake, n * sizeof(net_ap_t));
    return n;
}

bool net_time_synced(void) { return net_state() == NET_CONNECTED; }

/* ---- Async fetch --------------------------------------------------------- */

typedef struct {
    char *url;
    char *body;
    size_t len;
    net_fetch_cb_t cb;
    void *user;
    volatile bool *alive;
} fetch_t;

static void fetch_deliver(void *arg)
{
    fetch_t *f = (fetch_t *)arg;
    if (f->alive == NULL || *f->alive) f->cb(f->body, f->len, f->user);
    free(f->body);
    free(f->url);
    free(f);
}

void net_fetch(const char *url, net_fetch_cb_t cb, void *user, volatile bool *alive)
{
    fetch_t *f = (fetch_t *)calloc(1, sizeof(fetch_t));
    f->url     = strdup(url);
    f->cb      = cb;
    f->user    = user;
    f->alive   = alive;
    f->body    = net_http_get(url, &f->len); /* the simulator just blocks */
    lv_async_call(fetch_deliver, f);
}

char *net_http_get(const char *url, size_t *len)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "curl -sfL --max-time 10 '%s'", url);
    FILE *p = popen(cmd, "r");
    if (p == NULL) return NULL;
    size_t cap = 8192, n = 0;
    char *buf = malloc(cap);
    size_t r;
    while (buf && (r = fread(buf + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    int rc = pclose(p);
    if (buf == NULL || rc != 0 || n == 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (len) *len = n;
    return buf;
}
