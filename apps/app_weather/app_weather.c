/* Placeholder until v0.4, when the real module replaces this file. */
#include "app_weather.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
        "> atmos.scan ............ NOT INSTALLED\n"
        "> scheduled for firmware v0.4 (brings up Wi-Fi)\n"
        "\n"
        "PLANNED PAYLOAD\n"
        "  - Open-Meteo feed, no API key\n"
        "  - current conditions, 24h trace, 7-day strip\n"
        "  - Wi-Fi setup, SNTP clock sync, timezone setting\n"
        "  - offline cache of the last good reading");
}

static deck_app_t s_app = {
    .name     = "WEATHER",
    .tagline  = "ATMOS SCAN",
    .icon     = ICON_WEATHER,
    .eta      = "v0.4",
    .on_start = start,
};

deck_app_t *app_weather(void) { return &s_app; }
