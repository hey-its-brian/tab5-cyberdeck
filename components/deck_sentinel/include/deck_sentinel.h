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

#include "sentinel_core.h"
#include "sentinel_proto.h"
#include "sentinel_store.h"

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

#ifdef __cplusplus
}
#endif
