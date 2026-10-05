/* Internal to deck_core: power-on self test animation. */
#pragma once

#include <stdbool.h>

void deck_boot_run(void);
void deck_boot_skip(void);
bool deck_boot_active(void);
