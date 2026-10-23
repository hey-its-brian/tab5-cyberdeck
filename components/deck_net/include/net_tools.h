/*
 * Reachability tools for NET TOOLS: ping, DNS, traceroute, a TCP port check.
 * All of them block, so run them through net_tools_run(), which calls the
 * job on a worker task and the completion on the LVGL task.
 *
 * ICMP goes through a raw socket on the deck (lwIP) and an unprivileged
 * ICMP datagram socket in the simulator (macOS), the same code otherwise.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;        /* a reply came back in time */
    int seq;
    int ttl;        /* of the reply, 0 if unknown */
    float ms;
    char from[16];  /* who answered (dotted quad) */
} net_ping_reply_t;

/* Resolve `host` (name or dotted quad) to an IPv4 address. */
bool net_resolve(const char *host, char *ip, size_t n);

/* Send `count` echo requests, waiting `timeout_ms` for each. Fills out[0..count-1]
 * and returns how many replies came back. */
int net_ping(const char *host, int count, int timeout_ms, net_ping_reply_t *out);

typedef struct {
    int hop;
    char ip[16];    /* "" when nothing answered */
    float ms;
    bool reached;   /* this hop is the destination */
} net_hop_t;

/* One probe per hop, up to `max_hops`. Returns the number of hops filled;
 * the last one has `reached` set when the destination answered. */
int net_trace(const char *host, int max_hops, int timeout_ms, net_hop_t *out);

/* TCP connect with a timeout; `ms` gets the connect time. */
bool net_port_open(const char *host, int port, int timeout_ms, float *ms);

/* ---- LAN discovery ------------------------------------------------------- */

typedef struct {
    char ip[16];
    char name[48];     /* mDNS host or instance name, "" if unknown */
    char service[24];  /* first service type seen, e.g. "_http" */
    uint8_t mac[6];
    bool has_mac;
    float ms;          /* ping round trip, 0 when found by mDNS only */
    bool self;         /* this deck */
} net_host_t;

/* Ping sweep of the deck's /24 plus an mDNS browse of common services,
 * merged by address and sorted. `progress(done, total, user)` is called
 * from the worker as the sweep advances (may be NULL). Returns the count. */
int net_lan_scan(net_host_t *out, int max, void (*progress)(int done, int total, void *user), void *user);

/* Vendor from a MAC prefix (a short built-in table), or "". */
const char *net_oui_vendor(const uint8_t mac[6]);

/* Platform hooks, implemented by net_tab5.c and sim/net_sim.c. */
bool net_local_ip(uint32_t *ip, uint32_t *mask);          /* host byte order */
int  net_mdns_hosts(net_host_t *out, int max, int timeout_ms);
bool net_arp_mac(const char *ip, uint8_t mac[6]);

/* Run `job(user)` on a worker task; afterwards `done(user)` runs on the
 * LVGL task. One job at a time; returns false while one is running. */
bool net_tools_run(void (*job)(void *user), void (*done)(void *user), void *user);
bool net_tools_busy(void);

#ifdef __cplusplus
}
#endif
