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
    if (argc > 2 && strcmp(argv[2], "lan") == 0) {
        static net_host_t hosts[64];
        int hn = net_lan_scan(hosts, 64, NULL, NULL);
        printf("lan: %d hosts\n", hn);
        for (int i = 0; i < hn; i++) printf("  %-15s %s\n", hosts[i].ip, hosts[i].name);
    }
    return got > 0 ? 0 : 1;
}

/* Platform hooks the LAN sweep expects; the sim proper has real ones. */
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
bool net_local_ip(uint32_t *ip, uint32_t *mask)
{
    struct ifaddrs *ifs = NULL, *i;
    bool ok = false;
    if (getifaddrs(&ifs) != 0) return false;
    for (i = ifs; i && !ok; i = i->ifa_next) {
        if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        *ip   = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        *mask = ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
        ok    = true;
    }
    freeifaddrs(ifs);
    return ok;
}
int net_mdns_hosts(net_host_t *out, int max, int timeout_ms) { (void)out; (void)max; (void)timeout_ms; return 0; }
bool net_arp_mac(const char *ip, uint8_t mac[6]) { (void)ip; (void)mac; return false; }
