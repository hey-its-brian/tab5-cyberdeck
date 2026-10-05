/* Simulator: load font files from assets/fonts at first use. */
#include "deck_assets.h"

#include <stdio.h>
#include <stdlib.h>

static const char *s_files[] = {
    [DECK_ASSET_FONT_DISPLAY] = "Orbitron.ttf",
    [DECK_ASSET_FONT_MONO]    = "ShareTechMono-Regular.ttf",
    [DECK_ASSET_FONT_ICONS]   = "icons.ttf",
    [DECK_ASSET_FONT_TERM]    = "term.ttf",
};

static uint8_t *s_data[4];
static size_t s_size[4];

const uint8_t *deck_asset(deck_asset_t id, size_t *size)
{
    if (s_data[id] == NULL) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", DECK_ASSET_DIR, s_files[id]);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            fprintf(stderr, "[sim] missing asset %s\n", path);
            *size = 0;
            return NULL;
        }
        fseek(f, 0, SEEK_END);
        s_size[id] = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        s_data[id] = malloc(s_size[id]);
        if (fread(s_data[id], 1, s_size[id], f) != s_size[id]) {
            fprintf(stderr, "[sim] short read %s\n", path);
        }
        fclose(f);
    }
    *size = s_size[id];
    return s_data[id];
}
