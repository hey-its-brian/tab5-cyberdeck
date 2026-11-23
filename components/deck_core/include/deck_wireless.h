/*
 * WIRELESS TOOLS: which of two apps the deck boots with. NET TOOLS is the
 * deck's own Wi-Fi put to work (scan, ping, LAN, speed); SENTINEL is the
 * radio board suite, with those tools inside it. One or the other is
 * registered at boot, never both, so a deck on NET TOOLS never powers the
 * rear bus or starts the board's tasks. Chosen in SETTINGS > APPLICATIONS,
 * saved as "wireless" (0 NET TOOLS, 1 SENTINEL), applied at the next
 * restart.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True for SENTINEL. With nothing saved, a deck that had RF WATCH switched
 * on (v1.5's "app_rf") gets SENTINEL; every other deck gets NET TOOLS. */
bool deck_wireless_sentinel(void);

#ifdef __cplusplus
}
#endif
