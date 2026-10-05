/*
 * deck_net: Wi-Fi station over the Tab5's ESP32-C6, SNTP clock sync and a
 * small HTTPS GET helper. The simulator has its own implementation
 * (sim/net_sim.c) so the UI can be exercised without a radio.
 *
 * All functions are safe to call from the LVGL task; slow work (bringing up
 * the C6 link, connecting, HTTP) happens on deck_net's own task or the
 * caller's worker task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_STARTING,     /* bringing up the link to the C6 */
    NET_NO_RADIO,     /* the C6 did not answer */
    NET_IDLE,         /* radio up, no network configured */
    NET_CONNECTING,
    NET_CONNECTED,
} net_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} net_ap_t;

void net_init(void);

net_state_t net_state(void);
const char *net_state_name(net_state_t s);
const char *net_ssid(void);        /* configured network, "" if none */
const char *net_ip(void);          /* "" until connected */
int         net_rssi(void);        /* dBm, 0 when not connected */
int         net_bars(void);        /* 0..4 */

/* Save credentials and (re)connect. `pass` may be "" for open networks. */
void net_connect(const char *ssid, const char *pass);
void net_forget(void);

/* Asynchronous scan; poll net_scan_done(), then read the results. */
bool   net_scan_start(void);
bool   net_scan_done(void);
size_t net_scan_results(net_ap_t *out, size_t max);

/* True once SNTP has set the clock this boot. */
bool net_time_synced(void);

/* Blocking HTTPS/HTTP GET into a malloc'd, NUL-terminated buffer (caller
 * frees). Call from a worker task, never the LVGL task. NULL on failure. */
char *net_http_get(const char *url, size_t *len);

/* Non-blocking GET for UI code: runs on a worker task and calls `cb` on the
 * LVGL task with the body (NULL on failure). The body is freed after `cb`
 * returns. `alive` lets the caller cancel: if *alive is false by the time
 * the result arrives, `cb` is skipped (e.g. the module was closed). */
typedef void (*net_fetch_cb_t)(char *body, size_t len, void *user);
void net_fetch(const char *url, net_fetch_cb_t cb, void *user, volatile bool *alive);

#ifdef __cplusplus
}
#endif
