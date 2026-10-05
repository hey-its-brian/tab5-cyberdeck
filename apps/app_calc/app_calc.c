/* Placeholder until v0.3, when the real module replaces this file. */
#include "app_calc.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
        "> numeric.core .......... NOT INSTALLED\n"
        "> scheduled for firmware v0.3\n"
        "\n"
        "PLANNED PAYLOAD\n"
        "  - expression engine: ( ) ^ % sin cos sqrt log, ans recall\n"
        "  - scrolling history tape, Up/Down to reuse results\n"
        "  - programmer mode: hex dec bin oct, bitwise ops\n"
        "  - touch keypad that hides when you type");
}

static deck_app_t s_app = {
    .name     = "CALC",
    .tagline  = "NUMERIC CORE",
    .icon     = ICON_CALCULATOR,
    .eta      = "v0.3",
    .on_start = start,
};

deck_app_t *app_calc(void) { return &s_app; }
