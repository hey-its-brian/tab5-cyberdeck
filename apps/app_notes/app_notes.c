/* Placeholder until v0.2, when the real module replaces this file. */
#include "app_notes.h"

#include "deck_icons.h"
#include "deck_shell.h"

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    return deck_stub_screen(self, parent,
        "> datastore.md .......... NOT INSTALLED\n"
        "> scheduled for firmware v0.2\n"
        "\n"
        "PLANNED PAYLOAD\n"
        "  - file browser for /sdcard/notes: new, rename, delete\n"
        "  - monospace editor with autosave and Ctrl+S\n"
        "  - live preview: headings, lists, quotes, code, tables\n"
        "  - Ctrl+P flips edit and preview, or split side by side");
}

static deck_app_t s_app = {
    .name     = "NOTES",
    .tagline  = "MARKDOWN I/O",
    .icon     = ICON_MARKDOWN,
    .eta      = "v0.2",
    .on_start = start,
};

deck_app_t *app_notes(void) { return &s_app; }
