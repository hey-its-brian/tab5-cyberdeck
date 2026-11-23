/*
 * NET TOOLS: the deck's own Wi-Fi put to work, as a module of its own when
 * SENTINEL is not chosen (deck_wireless.h). The screens are SENTINEL's
 * network screens on SENTINEL's frame, started without a log, so there is no
 * mode chip and no alert line, and nothing here touches the radio board.
 */
#include "app_sentinel.h"

#include "deck_icons.h"
#include "sn_ui.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    sn_frame_start(parent, &sn_net_home, NULL);
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    sn_frame_stop();
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    return sn_frame_key(k);
}

static bool on_back(deck_app_t *self)
{
    (void)self;
    return sn_frame_back();
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
    .on_stop    = stop,
    .on_key     = on_key,
    .on_back    = on_back,
};

deck_app_t *app_net(void) { return &s_app; }
