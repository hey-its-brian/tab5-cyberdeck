/*
 * deck_net for the simulator: a pretend radio with a few networks, and real
 * HTTP through curl so the WEATHER module can fetch live data on the Mac.
 */
#include "deck_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net_tools.h"

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

/* A believable apartment block for the scanner. */
static const net_bss_t s_fake_bss[] = {
    {"NIGHTCITY-5G", {0x3c, 0x7c, 0x3f, 0x12, 0x34, 0x56}, 6, -41, "WPA2/3"},
    {"NIGHTCITY-5G", {0x3c, 0x7c, 0x3f, 0x12, 0x34, 0x57}, 11, -58, "WPA2/3"},
    {"rikers_iot", {0xa4, 0x2b, 0xb0, 0xaa, 0x01, 0x02}, 1, -47, "WPA2"},
    {"", {0xa4, 0x2b, 0xb0, 0xaa, 0x01, 0x03}, 1, -48, "WPA2"},
    {"xfinitywifi", {0x00, 0x1d, 0xd0, 0x99, 0x00, 0x11}, 6, -72, "OPEN"},
    {"Arasaka Guest", {0xd8, 0x47, 0x32, 0x00, 0x00, 0x01}, 6, -66, "WPA2"},
    {"Militech-Ops", {0xd8, 0x47, 0x32, 0x00, 0x00, 0x02}, 11, -80, "WPA3"},
    {"NETGEAR77", {0x9c, 0x3d, 0xcf, 0x77, 0x77, 0x77}, 3, -83, "WPA2"},
    {"Printer-Direct", {0x00, 0x80, 0x92, 0x01, 0x02, 0x03}, 9, -75, "OPEN"},
    {"Maelstrom", {0x2c, 0xf0, 0x5d, 0x55, 0x55, 0x55}, 13, -88, "WEP"},
};

size_t net_scan_bss(net_bss_t *out, size_t max)
{
    size_t n = sizeof(s_fake_bss) / sizeof(s_fake_bss[0]);
    if (n > max) n = max;
    memcpy(out, s_fake_bss, n * sizeof(net_bss_t));
    return n;
}

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

/* curl does the talking; the body goes through a temp file to stay clear of
 * shell quoting, and the status code rides on the last line of the output. */
char *net_http_request(const char *method, const char *url, const char *user, const char *pass, const char *body,
                       size_t body_len, int *status, size_t *len)
{
    if (status) *status = 0;
    char tmp[] = "/tmp/deck_sim_body_XXXXXX";
    int fd     = -1;
    if (body) {
        fd = mkstemp(tmp);
        if (fd < 0) return NULL;
        if (write(fd, body, body_len) != (ssize_t)body_len) {
            close(fd);
            unlink(tmp);
            return NULL;
        }
        close(fd);
    }
    char auth[256] = "";
    if (user && user[0]) snprintf(auth, sizeof(auth), "-u '%s:%s'", user, pass ? pass : "");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "curl -s --max-time 20 -X %s %s -H 'Accept: application/json' %s %s%s -w '\\n%%{http_code}' '%s'", method,
             auth, body ? "-H 'Content-Type: application/json'" : "", body ? "--data-binary @" : "", body ? tmp : "",
             url);
    FILE *p = popen(cmd, "r");
    if (p == NULL) {
        if (fd >= 0) unlink(tmp);
        return NULL;
    }
    size_t cap = 8192, n = 0;
    char *buf  = malloc(cap);
    size_t r;
    while (buf && (r = fread(buf + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    pclose(p);
    if (fd >= 0) unlink(tmp);
    if (buf == NULL) return NULL;
    buf[n] = '\0';
    char *nl = n ? strrchr(buf, '\n') : NULL;
    if (nl == NULL) {
        free(buf);
        return NULL;
    }
    if (status) *status = atoi(nl + 1);
    *nl = '\0';
    if (len) *len = (size_t)(nl - buf);
    if (status && *status == 0) {
        free(buf);
        return NULL;
    }
    return buf;
}

void net_http_idle(void) {}

/* ---- Throughput, through curl ------------------------------------------- */

static bool curl_numbers(const char *cmd, double *a, double *b)
{
    FILE *p = popen(cmd, "r");
    if (p == NULL) return false;
    char line[128] = "";
    char *got      = fgets(line, sizeof(line), p);
    pclose(p);
    return got && sscanf(line, "%lf %lf", a, b) == 2;
}

bool net_speed_download(const char *url, size_t max_bytes, int timeout_ms, size_t *bytes, float *ms)
{
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "curl -s -o /dev/null --max-time %d -r 0-%zu -w '%%{size_download} %%{time_total}' '%s'",
             timeout_ms / 1000 + 1, max_bytes - 1, url);
    double sz = 0, t = 0;
    if (!curl_numbers(cmd, &sz, &t)) return false;
    *bytes = (size_t)sz;
    *ms    = (float)(t * 1000.0);
    return *bytes > 0;
}

bool net_speed_upload(const char *url, size_t bytes, int timeout_ms, float *ms)
{
    char tmp[] = "/tmp/deck_sim_up_XXXXXX";
    int fd     = mkstemp(tmp);
    if (fd < 0) return false;
    char *z = calloc(1, 65536);
    for (size_t left = bytes; z && left; left -= left < 65536 ? left : 65536) {
        if (write(fd, z, left < 65536 ? left : 65536) < 0) break;
    }
    free(z);
    close(fd);
    char cmd[700];
    snprintf(cmd, sizeof(cmd),
             "curl -s -o /dev/null --max-time %d -X POST -H 'Content-Type: application/octet-stream' --data-binary @%s "
             "-w '%%{size_upload} %%{time_total}' '%s'",
             timeout_ms / 1000 + 1, tmp, url);
    double sz = 0, t = 0;
    bool ok   = curl_numbers(cmd, &sz, &t) && (size_t)sz == bytes;
    unlink(tmp);
    *ms = (float)(t * 1000.0);
    return ok;
}

/* ---- LAN discovery hooks ------------------------------------------------- */

bool net_local_ip(uint32_t *ip, uint32_t *mask)
{
    struct ifaddrs *ifs = NULL, *i;
    bool ok = false;
    if (getifaddrs(&ifs) != 0) return false;
    for (i = ifs; i && !ok; i = i->ifa_next) {
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!(i->ifa_flags & IFF_UP) || strncmp(i->ifa_name, "utun", 4) == 0) continue;
        *ip   = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        *mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        ok    = true;
    }
    freeifaddrs(ifs);
    return ok;
}

/* A believable set of neighbours; the real browse needs the deck. */
int net_mdns_hosts(net_host_t *out, int max, int timeout_ms)
{
    (void)timeout_ms;
    static const net_host_t fake[] = {
        {"192.168.1.1", "router", "_http", {0x9c, 0x3d, 0xcf, 0x77, 0x77, 0x77}, true, 0, false},
        {"192.168.1.20", "printer", "_ipp", {0x00, 0x80, 0x92, 0x01, 0x02, 0x03}, true, 0, false},
        {"192.168.1.42", "living-room-tv", "_airplay", {0xd0, 0x59, 0xe4, 0x10, 0x20, 0x30}, true, 0, false},
        {"192.168.1.77", "deck", "_http", {0x24, 0x0a, 0xc4, 0xaa, 0xbb, 0xcc}, true, 0, false},
        {"192.168.1.90", "hue-bridge", "_hap", {0x00, 0x17, 0x88, 0x11, 0x22, 0x33}, true, 0, false},
    };
    int n = (int)(sizeof(fake) / sizeof(fake[0]));
    if (n > max) n = max;
    memcpy(out, fake, (size_t)n * sizeof(fake[0]));
    return n;
}

bool net_arp_mac(const char *ip, uint8_t mac[6])
{
    (void)ip;
    (void)mac;
    return false;
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
