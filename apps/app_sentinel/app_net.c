/*
 * NET TOOLS: the deck's own Wi-Fi put to work, as a module of its own when
 * SENTINEL is not chosen (deck_wireless.h). The screens are SENTINEL's
 * network screens on SENTINEL's frame, started without a log, so there is no
 * mode chip and no alert line, and nothing here touches the radio board.
 */
#include "app_sentinel.h"

#include "deck_icons.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    (void)parent;
    return false; /* its screens arrive with the next commit */
}

static deck_app_t s_app = {
    .name       = "NET TOOLS",
    .tagline    = "WI-FI + LAN",
    .icon       = ICON_LAN,
    .eta        = NULL,
    .hotkey     = HID_LETTER('R'),
    .id         = "net",
    .desc       = "Wi-Fi scan, ping, LAN sweep, speed test",
    .optional   = true,
    .default_on = true,
    .on_start   = start,
};

deck_app_t *app_net(void) { return &s_app; }
