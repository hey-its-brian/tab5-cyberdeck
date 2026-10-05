/* Placeholder until v0.7, when the real module replaces this file. */
#include "app_term.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
        "> uplink.ssh ............ NOT INSTALLED\n"
        "> scheduled for firmware v0.7\n"
        "\n"
        "PLANNED PAYLOAD\n"
        "  - wolfSSH client, ed25519 keys from SD, known_hosts pinning\n"
        "  - xterm-256color emulator, 160x45 grid, PSRAM scrollback\n"
        "  - full Ctrl/Alt/arrow passthrough; Alt+Esc returns to deck\n"
        "  - saved host profiles with one-tap connect");
}

static deck_app_t s_app = {
    .name     = "TERMINAL",
    .tagline  = "SSH UPLINK",
    .icon     = ICON_CONSOLE,
    .eta      = "v0.7",
    .on_start = start,
};

deck_app_t *app_term(void) { return &s_app; }
