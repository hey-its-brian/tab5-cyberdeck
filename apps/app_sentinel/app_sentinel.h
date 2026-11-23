#pragma once
#include "deck_app.h"

/* The two WIRELESS TOOLS apps (deck_wireless.h): one of them is registered at boot. */
deck_app_t *app_sentinel(void);
deck_app_t *app_net(void);
