/* Host check for the NET TOOLS helpers against the real internet:
 *   cmake --build sim/build --target nettools_test && sim/build/nettools_test [host] */
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "net_tools.h"

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "one.one.one.one";
    lv_init();
    char ip[16] = "";
    printf("resolve %s -> %s\n", host, net_resolve(host, ip, sizeof(ip)) ? ip : "FAILED");

    net_ping_reply_t r[4];
    int got = net_ping(host, 4, 1500, r);
    printf("ping: %d/4 replies\n", got);
    for (int i = 0; i < 4; i++) {
        if (r[i].ok) printf("  seq %d from %s ttl %d %.1f ms\n", r[i].seq, r[i].from, r[i].ttl, r[i].ms);
        else printf("  seq %d timeout\n", r[i].seq);
    }

    net_hop_t h[12];
    int n = net_trace(host, 12, 1500, h);
    printf("trace: %d hops\n", n);
    for (int i = 0; i < n; i++) printf("  %2d %-16s %s%.1f ms\n", h[i].hop, h[i].ip[0] ? h[i].ip : "*", h[i].reached ? "reached " : "", h[i].ms);

    float ms = 0;
    printf("port 443: %s (%.1f ms)\n", net_port_open(host, 443, 2000, &ms) ? "open" : "closed", ms);
    printf("port 9: %s (%.1f ms)\n", net_port_open(host, 9, 1500, &ms) ? "open" : "closed", ms);
    return got > 0 ? 0 : 1;
}
