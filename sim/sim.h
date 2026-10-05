/* Simulator-only hooks. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Queue a keyboard event exactly as the Tab5 Keyboard would report it. */
void sim_push_key(uint8_t code, uint8_t mods, bool pressed);
