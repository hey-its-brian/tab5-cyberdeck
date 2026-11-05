#pragma once

#include "deck_app.h"

deck_app_t *app_sys(void);
/* Open SETTINGS on the APPLICATIONS tab with `focus`'s switch selected (NULL:
 * no particular app). The APPS screen calls it for a greyed tile. */
void app_sys_open_applications(const deck_app_t *focus);
