/*
 * RF WATCH: the RF Sentinel carrier on the Tab5's M5-Bus (a Stamp C5 with a
 * CC1101, an nRF24 and a GPS). Scheduled; shows its manifest until the
 * board exists.
 */
#include "app_rf.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
                            "> SUB-GHZ SWEEP ......... 433 MHz band, 21 channels, live RSSI\n"
                            "> 2.4 GHZ OCCUPANCY ..... 126 channels from the nRF24 carrier detect\n"
                            "> GNSS WATCH ............ fix, satellites, C/N0, interference alerts\n"
                            "> WI-FI PRESENCE ........ dual-band counts from the C5\n"
                            "> EVENTS ................ baseline alerts, trackers, skimmers, cameras\n"
                            "\n"
                            "NEEDS THE SENTINEL BOARD ON THE M5-BUS. SCHEDULED FOR v1.5.");
}

static deck_app_t s_app = {
    .name     = "RF WATCH",
    .tagline  = "SENTINEL BOARD",
    .icon     = ICON_ANTENNA,
    .eta      = "v1.5",
    .hotkey   = HID_LETTER('A'),
    .on_start = start,
};

deck_app_t *app_rf(void) { return &s_app; }
