#include "net_tools.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#else
#include <pthread.h>
#include "lvgl.h"
#endif

/* ---- ICMP on the wire ---------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
} icmp_hdr_t;

#define ICMP_ECHO_REPLY 0
#define ICMP_ECHO 8
#define ICMP_TIME_EXCEEDED 11
#define ICMP_UNREACHABLE 3
#define PAYLOAD 32

static uint16_t checksum(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t sum     = 0;
    while (n > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        n -= 2;
    }
    if (n) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int icmp_socket(void)
{
#ifdef ESP_PLATFORM
    return socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
#else
    return socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP); /* unprivileged on macOS */
#endif
}

/* Milliseconds since boot-ish, as a double: the epoch in a float has a
 * resolution of two minutes. */
static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

bool net_resolve(const char *host, char *ip, size_t n)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) return false;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sa->sin_addr, ip, (socklen_t)n);
    freeaddrinfo(res);
    return true;
}

/* Send one echo request with the given TTL and wait for an answer. Returns
 * the ICMP type that came back (0 reply, 11 time exceeded, 3 unreachable),
 * -1 on timeout. `from` gets the sender. */
static int probe(int sock, struct sockaddr_in *to, uint16_t id, uint16_t seq, int ttl, int timeout_ms, char *from,
                 size_t from_n, float *ms, int *reply_ttl)
{
    uint8_t pkt[sizeof(icmp_hdr_t) + PAYLOAD];
    icmp_hdr_t *h = (icmp_hdr_t *)pkt;
    h->type       = ICMP_ECHO;
    h->code       = 0;
    h->checksum   = 0;
    h->id         = htons(id);
    h->seq        = htons(seq);
    for (int i = 0; i < PAYLOAD; i++) pkt[sizeof(*h) + i] = (uint8_t)('A' + i % 26);
    h->checksum = htons(checksum(pkt, sizeof(pkt)));

    if (ttl > 0) setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
    double t0 = now_ms();
    if (sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)to, sizeof(*to)) < 0) return -1;

    for (;;) {
        double left = (double)timeout_ms - (now_ms() - t0);
        if (left <= 0) return -1;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sock, &rf);
        struct timeval tv = {.tv_sec = (int)left / 1000, .tv_usec = ((int)left % 1000) * 1000};
        if (select(sock + 1, &rf, NULL, NULL, &tv) <= 0) return -1;

        uint8_t buf[256];
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n        = (int)recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
        if (n < (int)sizeof(icmp_hdr_t)) continue;
        /* Both sockets hand us the IP header first; its length is in the low nibble. */
        int ihl = (buf[0] >> 4) == 4 ? (buf[0] & 0x0F) * 4 : 0;
        if (n < ihl + (int)sizeof(icmp_hdr_t)) continue;
        const icmp_hdr_t *r = (const icmp_hdr_t *)(buf + ihl);
        bool ours           = false;
        if (r->type == ICMP_ECHO_REPLY) {
            /* macOS rewrites the id on datagram sockets, so match on seq. */
            ours = ntohs(r->seq) == seq;
        } else if (r->type == ICMP_TIME_EXCEEDED || r->type == ICMP_UNREACHABLE) {
            /* The original IP header and 8 bytes of our echo follow. */
            const uint8_t *inner = buf + ihl + sizeof(icmp_hdr_t);
            int inner_ihl        = (inner[0] & 0x0F) * 4;
            if (n >= ihl + (int)sizeof(icmp_hdr_t) + inner_ihl + 8) {
                const icmp_hdr_t *orig = (const icmp_hdr_t *)(inner + inner_ihl);
                ours                   = orig->type == ICMP_ECHO && ntohs(orig->seq) == seq;
            }
        }
        if (!ours) continue;
        *ms = (float)(now_ms() - t0);
        inet_ntop(AF_INET, &src.sin_addr, from, (socklen_t)from_n);
        if (reply_ttl) *reply_ttl = ihl ? buf[8] : 0;
        return r->type;
    }
}

static bool target(const char *host, struct sockaddr_in *to)
{
    char ip[16];
    if (!net_resolve(host, ip, sizeof(ip))) return false;
    memset(to, 0, sizeof(*to));
    to->sin_family = AF_INET;
    return inet_pton(AF_INET, ip, &to->sin_addr) == 1;
}

int net_ping(const char *host, int count, int timeout_ms, net_ping_reply_t *out)
{
    struct sockaddr_in to;
    if (!target(host, &to)) return -1;
    int sock = icmp_socket();
    if (sock < 0) return -1;
    uint16_t id = (uint16_t)(now_ms() * 7.0f) | 1;
    int got     = 0;
    for (int i = 0; i < count; i++) {
        net_ping_reply_t *r = &out[i];
        memset(r, 0, sizeof(*r));
        r->seq   = i + 1;
        int type = probe(sock, &to, id, (uint16_t)(i + 1), 0, timeout_ms, r->from, sizeof(r->from), &r->ms, &r->ttl);
        r->ok    = type == ICMP_ECHO_REPLY;
        if (r->ok) got++;
        if (i + 1 < count && r->ok && r->ms < 1000) usleep((useconds_t)((1000 - (int)r->ms) * 1000));
    }
    close(sock);
    return got;
}

int net_trace(const char *host, int max_hops, int timeout_ms, net_hop_t *out)
{
    struct sockaddr_in to;
    if (!target(host, &to)) return -1;
    int sock = icmp_socket();
    if (sock < 0) return -1;
    uint16_t id = (uint16_t)(now_ms() * 13.0f) | 1;
    int n       = 0;
    for (int ttl = 1; ttl <= max_hops; ttl++) {
        net_hop_t *h = &out[n++];
        memset(h, 0, sizeof(*h));
        h->hop   = ttl;
        int type = probe(sock, &to, id, (uint16_t)ttl, ttl, timeout_ms, h->ip, sizeof(h->ip), &h->ms, NULL);
        if (type == ICMP_ECHO_REPLY) {
            h->reached = true;
            break;
        }
        if (type == ICMP_UNREACHABLE) break;
    }
    close(sock);
    return n;
}

bool net_port_open(const char *host, int port, int timeout_ms, float *ms)
{
    struct sockaddr_in to;
    if (!target(host, &to)) return false;
    to.sin_port = htons((uint16_t)port);
    int sock    = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);
    double t0 = now_ms();
    bool ok   = false;
    int rc   = connect(sock, (struct sockaddr *)&to, sizeof(to));
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(sock, &wf);
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        if (select(sock + 1, NULL, &wf, NULL, &tv) > 0) {
            int err      = 0;
            socklen_t el = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = err == 0;
        }
    }
    if (ms) *ms = (float)(now_ms() - t0);
    close(sock);
    return ok;
}

/* ---- LAN discovery ------------------------------------------------------- */

typedef struct {
    uint8_t oui[3];
    const char *name;
} oui_t;

/* A short table of the makers most likely on a home network. */
static const oui_t s_oui[] = {
    {{0xA4, 0x83, 0xE7}, "Apple"},      {{0xF0, 0x18, 0x98}, "Apple"},      {{0x3C, 0x22, 0xFB}, "Apple"},
    {{0xA8, 0x51, 0xAB}, "Apple"},      {{0xBC, 0xD0, 0x74}, "Apple"},      {{0x24, 0x0A, 0xC4}, "Espressif"},
    {{0x30, 0xAE, 0xA4}, "Espressif"},  {{0x84, 0xCC, 0xA8}, "Espressif"},  {{0xA4, 0xCF, 0x12}, "Espressif"},
    {{0x7C, 0xDF, 0xA1}, "Espressif"},  {{0x48, 0xE7, 0x29}, "Espressif"},  {{0xEC, 0xDA, 0x3B}, "Espressif"},
    {{0xB8, 0x27, 0xEB}, "Raspberry Pi"}, {{0xDC, 0xA6, 0x32}, "Raspberry Pi"}, {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
    {{0x2C, 0xCF, 0x67}, "Raspberry Pi"}, {{0xD8, 0x3A, 0xDD}, "Raspberry Pi"},
    {{0x18, 0xB4, 0x30}, "Google Nest"}, {{0xF4, 0xF5, 0xD8}, "Google"},    {{0x30, 0xFD, 0x38}, "Google"},
    {{0x44, 0x65, 0x0D}, "Amazon"},     {{0xFC, 0x65, 0xDE}, "Amazon"},     {{0x74, 0xC2, 0x46}, "Amazon"},
    {{0x0C, 0x47, 0xC9}, "Amazon"},     {{0x50, 0xC7, 0xBF}, "TP-Link"},    {{0x60, 0x32, 0xB1}, "TP-Link"},
    {{0xB0, 0xBE, 0x76}, "TP-Link"},    {{0x24, 0x5A, 0x4C}, "Ubiquiti"},   {{0x74, 0x83, 0xC2}, "Ubiquiti"},
    {{0xFC, 0xEC, 0xDA}, "Ubiquiti"},   {{0xA0, 0x40, 0xA0}, "Netgear"},    {{0x9C, 0x3D, 0xCF}, "Netgear"},
    {{0x00, 0x0E, 0x58}, "Sonos"},      {{0x48, 0xA6, 0xB8}, "Sonos"},      {{0xB8, 0xA1, 0x75}, "Roku"},
    {{0xD8, 0x31, 0x34}, "Roku"},       {{0x8C, 0x49, 0x62}, "Roku"},       {{0x00, 0x17, 0x88}, "Philips Hue"},
    {{0xEC, 0xB5, 0xFA}, "Philips Hue"}, {{0x34, 0x94, 0x54}, "Shelly"},    {{0xC4, 0x5B, 0xBE}, "Shelly"},
    {{0x2C, 0xAA, 0x8E}, "Wyze"},       {{0xD0, 0x3F, 0x27}, "Wyze"},       {{0xEC, 0x71, 0xDB}, "Reolink"},
    {{0x34, 0x78, 0xD7}, "Reolink"},    {{0xC0, 0x56, 0xE3}, "Hikvision"},  {{0x3C, 0xEF, 0x8C}, "Hikvision"},
    {{0xA0, 0xBD, 0x1D}, "Dahua"},      {{0x9C, 0x8E, 0xCD}, "Amcrest"},    {{0x00, 0x62, 0x6E}, "Ring"},
    {{0x54, 0xE0, 0x19}, "Ring"},       {{0x08, 0x02, 0x8E}, "Netgear Arlo"}, {{0xD0, 0x73, 0xD5}, "LIFX"},
    {{0x10, 0xD5, 0x61}, "Tuya"},       {{0xD8, 0x1F, 0x12}, "Tuya"},       {{0x68, 0x57, 0x2D}, "Tuya"},
    {{0x78, 0x11, 0xDC}, "Xiaomi"},     {{0x64, 0x90, 0xC1}, "Xiaomi"},     {{0x00, 0x11, 0x32}, "Synology"},
    {{0x24, 0x5E, 0xBE}, "QNAP"},       {{0x00, 0x1E, 0x8F}, "Canon"},      {{0x00, 0x80, 0x92}, "Brother"},
    {{0x00, 0x26, 0xAB}, "Epson"},      {{0x3C, 0x5A, 0xB4}, "Google"},     {{0x00, 0x1D, 0xD8}, "Microsoft"},
    {{0x98, 0x5F, 0xD3}, "Microsoft"},  {{0x7C, 0xBB, 0x8A}, "Nintendo"},   {{0x00, 0x1F, 0xA7}, "Sony"},
    {{0x00, 0x1E, 0xC2}, "Apple"},      {{0x14, 0x7D, 0xDA}, "Apple"},      {{0xDC, 0x56, 0xE7}, "Apple"},
    {{0x98, 0x01, 0xA7}, "Apple"},      {{0x8C, 0x85, 0x90}, "Apple"},      {{0x4C, 0x56, 0x9D}, "Apple"},
    {{0xE0, 0xB9, 0xE5}, "Technicolor"}, {{0x00, 0x1D, 0xD0}, "Arris"},     {{0xA8, 0x9F, 0xEC}, "Arris"},
    {{0x00, 0x1A, 0xA0}, "Dell"},       {{0xF4, 0x8E, 0x38}, "Dell"},       {{0x3C, 0xD9, 0x2B}, "HP"},
    {{0x00, 0x1B, 0x21}, "Intel"},      {{0x8C, 0x8D, 0x28}, "Intel"},      {{0x04, 0x4E, 0xAF}, "LG"},
    {{0x8C, 0x79, 0xF5}, "Samsung"},    {{0xD0, 0x59, 0xE4}, "Samsung"},    {{0x4C, 0xFC, 0xAA}, "Tesla"},
};

const char *net_oui_vendor(const uint8_t mac[6])
{
    for (size_t i = 0; i < sizeof(s_oui) / sizeof(s_oui[0]); i++) {
        if (memcmp(s_oui[i].oui, mac, 3) == 0) return s_oui[i].name;
    }
    /* Locally administered bit: a randomised phone or laptop address. */
    if (mac[0] & 0x02) return "private address";
    return "";
}

static int find_host(net_host_t *hosts, int n, const char *ip)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(hosts[i].ip, ip) == 0) return i;
    }
    return -1;
}

static int by_ip(const void *a, const void *b)
{
    const net_host_t *x = a, *y = b;
    struct in_addr ia, ib;
    inet_pton(AF_INET, x->ip, &ia);
    inet_pton(AF_INET, y->ip, &ib);
    uint32_t ha = ntohl(ia.s_addr), hb = ntohl(ib.s_addr);
    return ha < hb ? -1 : ha > hb;
}

int net_lan_scan(net_host_t *out, int max, void (*progress)(int, int, void *), void *user)
{
    uint32_t me = 0, mask = 0;
    if (!net_local_ip(&me, &mask) || max <= 0) return 0;
    if (mask == 0 || mask < 0xFFFFFF00u) mask = 0xFFFFFF00u; /* sweep at most a /24 */
    uint32_t base  = me & mask;
    uint32_t count = ~mask;                                   /* 255 for a /24 */
    int n          = 0;

    /* 1. Echo requests to the whole range, two passes, replies collected as
     *    they come. The id ties replies to this sweep; the seq is the host. */
    int sock = icmp_socket();
    if (sock >= 0) {
        /* Never block on a send: a full buffer or a blocked protocol just
         * means that host is skipped this pass. */
        int fl = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, fl | O_NONBLOCK);
        uint16_t id = (uint16_t)(now_ms() * 3.0) | 1;
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t h = 1; h < count; h++) {
                uint32_t ip = base | h;
                if (ip == me) continue;
                struct sockaddr_in to = {0};
                to.sin_family         = AF_INET;
                to.sin_addr.s_addr    = htonl(ip);
                uint8_t pkt[sizeof(icmp_hdr_t) + 8];
                icmp_hdr_t *hd = (icmp_hdr_t *)pkt;
                hd->type       = ICMP_ECHO;
                hd->code       = 0;
                hd->checksum   = 0;
                hd->id         = htons(id);
                hd->seq        = htons((uint16_t)h);
                memset(pkt + sizeof(*hd), 'd', 8);
                hd->checksum = htons(checksum(pkt, sizeof(pkt)));
                if (sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0 &&
                    (errno == EAGAIN || errno == ENOBUFS || errno == EWOULDBLOCK)) {
                    usleep(20000); /* let the stack drain, then move on */
                }
                if ((h & 15) == 0) {
                    usleep(4000); /* do not flood the C6 link */
                    if (progress) progress((int)(pass * count + h), (int)(2 * count), user);
                }
            }
            /* Collect for a while. */
            double t0 = now_ms();
            while (now_ms() - t0 < 1500) {
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(sock, &rf);
                struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
                if (select(sock + 1, &rf, NULL, NULL, &tv) <= 0) continue;
                uint8_t buf[128];
                struct sockaddr_in src;
                socklen_t sl = sizeof(src);
                int got      = (int)recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
                if (got < (int)sizeof(icmp_hdr_t)) continue;
                int ihl = (buf[0] >> 4) == 4 ? (buf[0] & 0x0F) * 4 : 0;
                if (got < ihl + (int)sizeof(icmp_hdr_t)) continue;
                const icmp_hdr_t *r = (const icmp_hdr_t *)(buf + ihl);
                if (r->type != ICMP_ECHO_REPLY) continue;
                char ip[16];
                inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
                if (find_host(out, n, ip) >= 0 || n >= max) continue;
                net_host_t *hst = &out[n++];
                memset(hst, 0, sizeof(*hst));
                snprintf(hst->ip, sizeof(hst->ip), "%s", ip);
                hst->ms = 1.0f; /* answered; the exact time of a sweep reply is not interesting */
            }
        }
        close(sock);
    }

    /* 2. mDNS names and services, merged by address. */
    net_host_t *found = calloc((size_t)max, sizeof(net_host_t));
    if (found) {
        int m = net_mdns_hosts(found, max, 2500);
        for (int i = 0; i < m; i++) {
            int at = find_host(out, n, found[i].ip);
            if (at < 0 && n < max) {
                at = n++;
                memset(&out[at], 0, sizeof(out[at]));
                snprintf(out[at].ip, sizeof(out[at].ip), "%s", found[i].ip);
            }
            if (at < 0) continue;
            if (!out[at].name[0]) snprintf(out[at].name, sizeof(out[at].name), "%s", found[i].name);
            if (!out[at].service[0]) snprintf(out[at].service, sizeof(out[at].service), "%s", found[i].service);
        }
        free(found);
    }

    /* 3. This deck, and the hardware addresses the ARP table learned. */
    char mine[16];
    struct in_addr a = {.s_addr = htonl(me)};
    inet_ntop(AF_INET, &a, mine, sizeof(mine));
    if (find_host(out, n, mine) < 0 && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].ip, sizeof(out[n].ip), "%s", mine);
        snprintf(out[n].name, sizeof(out[n].name), "this deck");
        out[n].self = true;
        n++;
    }
    for (int i = 0; i < n; i++) out[i].has_mac = !out[i].self && net_arp_mac(out[i].ip, out[i].mac);
    qsort(out, (size_t)n, sizeof(out[0]), by_ip);
    if (progress) progress(1, 1, user);
    return n;
}

/* ---- Worker -------------------------------------------------------------- */

static struct {
    void (*job)(void *);
    void (*done)(void *);
    void *user;
    volatile bool busy;
} s_run;

static void deliver(void *arg)
{
    (void)arg;
    void (*done)(void *) = s_run.done;
    void *user           = s_run.user;
    s_run.busy           = false;
    if (done) done(user);
}

#ifdef ESP_PLATFORM
static void worker(void *arg)
{
    (void)arg;
    s_run.job(s_run.user);
    lv_async_call(deliver, NULL);
    vTaskDelete(NULL);
}
#else
static void *worker(void *arg)
{
    (void)arg;
    s_run.job(s_run.user);
    lv_async_call(deliver, NULL);
    return NULL;
}
#endif

bool net_tools_busy(void) { return s_run.busy; }

bool net_tools_run(void (*job)(void *), void (*done)(void *), void *user)
{
    if (s_run.busy || job == NULL) return false;
    s_run.job  = job;
    s_run.done = done;
    s_run.user = user;
    s_run.busy = true;
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCore(worker, "nettools", 8192, NULL, 3, NULL, 0) != pdPASS) {
        s_run.busy = false;
        return false;
    }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) {
        s_run.busy = false;
        return false;
    }
    pthread_detach(t);
#endif
    return true;
}
