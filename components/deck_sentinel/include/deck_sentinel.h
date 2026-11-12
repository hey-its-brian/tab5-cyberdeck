/*
 * deck_sentinel: the RF Sentinel carrier board on the Tab5's rear M5-Bus.
 *
 * The board carries a Stamp C5 (ESP32-C5) that sweeps a CC1101 (433 MHz
 * band), an nRF24L01+ (2.4 GHz carrier detect), reads a GPS and scans Wi-Fi,
 * then streams what it saw as $RFS sentences over a UART. The Tab5 powers
 * the board (EXT5V on the bus), keeps the CC1101 front end in receive and
 * parses the stream into one state record that the RF WATCH app draws.
 *
 * Tab5 side of the bus (hardware/tab5-board/README.md in rf-sentinel):
 *   G6  UART TX to the C5        G7  UART RX from the C5     115200 8N1
 *   G16 E07 RX_EN (out, high)    G17 E07 TX_EN (out, low, never with G16)
 *   G45 E07 GDO0 (in)            G52 E07 GDO2 (in)           G4 nRF24 IRQ (in, low)
 *   5 V on bus pin 28 comes from EXT5V_EN on the PI4IOE5V6408 at 0x43, P2.
 *
 * The state record, the parser and everything else that is not a pin live in
 * sentinel_core (external/rf-sentinel/sentinel_core), shared with the C5
 * firmware and the other hosts. This header is the Tab5's side of it.
 *
 * sentinel_start() runs from RF WATCH's on_enable, sentinel_stop() from its
 * on_disable, both on the LVGL task. The reader task fills the state and
 * runs the core's link machine (HELLO, the baud rate, the arm lease);
 * sentinel_snapshot() copies the state out under the lock. A writer task
 * puts every event and captured frame on storage (sentinel_store.h). The
 * simulator provides a fake node that speaks the same sentences
 * (sim/sentinel_sim.c).
 */
#pragma once

#include "sentinel_blehost.h"
#include "sentinel_core.h"
#include "sentinel_proto.h"
#include "sentinel_store.h"
#include "sentinel_wifihost.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Milliseconds on the clock the state's timestamps use. */
uint32_t sentinel_now_ms(void);

/* Power the board, put the CC1101 in receive and start reading. Safe to call
 * twice. On the device the reader task stays for the life of the firmware;
 * sentinel_stop() lowers RX_EN, cuts EXT5V and leaves it idle. */
void sentinel_start(void);
void sentinel_stop(void);
bool sentinel_running(void);

/* Copy the current state out (thread safe). */
void sentinel_snapshot(sentinel_state_t *out);

/* A frame has arrived recently. */
static inline bool sentinel_link(const sentinel_state_t *s) { return sentinel_link_up(s, sentinel_now_ms()); }

/* Send a command to the C5: "CMD,HELLO", "CMD,PING", "CMD,QUIET", "CMD,STREAM",
 * "CMD,RATE,<ms>", "CMD,TX,<test>". Returns false when the board is off. */
bool sentinel_send(const char *payload);

/* RED TEAM mode: hold the node's arm lease (refreshed until dropped), or
 * drop it. The node refuses every transmit test without it, and lets it
 * lapse by itself if the refreshes stop. */
void sentinel_arm(bool on);
/* The node currently confirms the lease. */
static inline bool sentinel_armed(const sentinel_state_t *s)
{
    return sentinel_arm_active(&s->arm, sentinel_now_ms());
}

/* Record something the deck did or noticed, as an event from source "deck":
 * it shows in EVENTS and goes to the log like the node's own. */
void sentinel_note(bool warn, const char *text);

/* What reached storage and what did not (store losses plus a full queue). */
void sentinel_stats(sentinel_store_stats_t *out);

/* ---- Wi-Fi --------------------------------------------------------------- */

/* Everything the node has reported about Wi-Fi: access points (strongest
 * first, silent ones dropped), the latest dwell on each channel, the
 * waterfall and the drones. A copy, about 8 KB: give it static or PSRAM
 * storage. sentinel_tool("wifi") makes the node hop every channel. */
void sentinel_wifi_copy(sentinel_wifihost_t *out);

/* The honeypot: an open access point with this name on this channel and
 * nothing behind it. It transmits, so the node only runs it while it
 * confirms the arm lease (RED TEAM); in a SAFE session this does nothing.
 * sentinel_tool("watch") or the lease ending stops it. */
void sentinel_honeypot(uint8_t channel, const char *ssid);

/* ---- Bluetooth ----------------------------------------------------------- */

/* Which tool owns the node's own radio: "watch" (shared between the Wi-Fi
 * presence scan and a Bluetooth scan, the background state), "ble"
 * (Bluetooth full time), "wifi" (hopping every Wi-Fi channel) or "hpot"
 * (the honeypot, see above); "drive" belongs to sentinel_drive_start().
 * Asked until the node agrees;
 * sentinel_start() begins in "watch". */
void sentinel_tool(const char *mode);
/* Have the node stream every advertisement, written to a pcap file on the
 * SD card. Only does anything in the "ble" tool. */
void sentinel_capture(bool on);

/* The devices in earshot: a copy, devices silent for a minute dropped,
 * strongest first. The table is 4 KB: give it static or PSRAM storage. */
void sentinel_ble_table(sentinel_bletab_t *out);
/* What the track watch knows about a tracker; false if it is not one. */
bool sentinel_ble_track(const uint8_t addr[6], sentinel_track_t *out);
/* "This one is mine": no alerts for it. Saved in settings ("rf_mine"). */
bool sentinel_ble_mine(const uint8_t addr[6]);
void sentinel_ble_set_mine(const uint8_t addr[6], bool mine);

/* ---- Wardriving ---------------------------------------------------------- */

#define SENTINEL_DRIVE_RECENT 10
#define SENTINEL_DRIVE_TEXT   48

/* A drive, as the WARDRIVE screen shows it. The counts stay after it ends. */
typedef struct {
    bool     on;          /* a drive is running */
    bool     writing;     /* its two files are open on the SD card */
    bool     placed;      /* the GPS gives a place and a time, so sightings are being written */
    uint32_t started_ms;
    uint32_t wifi, ble;   /* access points and Bluetooth devices on file */
    uint32_t rows, survey_rows;
    uint32_t nofix, optout, priv; /* sightings not written: no place, an opted-out network, a rotating address */
    uint32_t lost;        /* rows that did not reach the card */
    float    distance_m;
    char     file[32];    /* the WiGLE file's name */
    int      recent_n;
    struct {
        bool   ble;
        int8_t rssi;
        char   text[SENTINEL_DRIVE_TEXT];
    } recent[SENTINEL_DRIVE_RECENT]; /* the last rows written, newest first */
} sentinel_drive_info_t;

/* Start a wardrive: the node hops every Wi-Fi channel with its Bluetooth
 * scan beside it (the "drive" tool, all of it listening), and what it hears
 * goes into sentinel/wigle-<time>.csv with the GPS position, in WiGLE's
 * format, plus sentinel/survey-<time>.csv for the two swept bands. The
 * rules for what is written are in sentinel_drive.h. SD card only.
 * `app_version` goes in the file's first line. Stop hands the radio back
 * ("watch") and closes the files; so does sentinel_stop(). */
void sentinel_drive_start(const char *app_version);
void sentinel_drive_stop(void);
void sentinel_drive_info(sentinel_drive_info_t *out);

#ifdef __cplusplus
}
#endif
