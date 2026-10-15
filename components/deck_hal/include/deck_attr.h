/* Placement attributes that compile to nothing in the simulator.
 *
 * DECK_PSRAM_BSS puts a large static buffer in PSRAM instead of internal RAM.
 * Early boot (ESP-Hosted's buffers, the idle tasks' stacks) only has the
 * first internal RAM region to draw from; when static data shrinks it too
 * far, IDF 5.4 hands the idle stack out of TCM and the stack check asserts
 * at every boot. Not for anything touched from an ISR. */
#pragma once

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define DECK_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define DECK_PSRAM_BSS
#endif
