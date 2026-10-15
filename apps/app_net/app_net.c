/*
 * NET TOOLS: scheduled. Shows its manifest until the module lands.
 */
#include "app_net.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> WI-FI SCANNER ......... networks, channels, signal, security\n"
                            "> LAN DISCOVERY ......... mDNS names and a ping sweep\n"
                            "> PING / DNS / TRACE .... reachability and the route out\n"
                            "> PORT CHECK ............ is that service listening\n"
                            "> SPEED TEST ............ down and up\n"
                            "\n"
                            "AWAITING UPLINK. SCHEDULED FOR v1.3.");
}

static deck_app_t s_app = {
    .name     = "NET TOOLS",
    .tagline  = "RECON SUITE",
    .icon     = ICON_RADAR,
    .eta      = "v1.3",
    .hotkey   = HID_LETTER('R'),
    .on_start = start,
};

deck_app_t *app_net(void) { return &s_app; }
