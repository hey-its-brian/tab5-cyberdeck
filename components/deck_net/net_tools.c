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
