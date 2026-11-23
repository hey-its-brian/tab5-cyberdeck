/* The WIRELESS TOOLS choice. See deck_wireless.h. */
#include "deck_wireless.h"

#include "deck_hal.h"

bool deck_wireless_sentinel(void) { return hal_cfg_get_i32("wireless", hal_cfg_get_i32("app_rf", 0) != 0) != 0; }
