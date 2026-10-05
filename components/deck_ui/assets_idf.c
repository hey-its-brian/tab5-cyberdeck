/* Device build: fonts are linked into flash by EMBED_FILES (see CMakeLists). */
#include "deck_assets.h"

extern const uint8_t orbitron_ttf_start[] asm("_binary_Orbitron_ttf_start");
extern const uint8_t orbitron_ttf_end[] asm("_binary_Orbitron_ttf_end");
extern const uint8_t mono_ttf_start[] asm("_binary_ShareTechMono_Regular_ttf_start");
extern const uint8_t mono_ttf_end[] asm("_binary_ShareTechMono_Regular_ttf_end");
extern const uint8_t icons_ttf_start[] asm("_binary_icons_ttf_start");
extern const uint8_t icons_ttf_end[] asm("_binary_icons_ttf_end");
extern const uint8_t term_ttf_start[] asm("_binary_term_ttf_start");
extern const uint8_t term_ttf_end[] asm("_binary_term_ttf_end");

const uint8_t *deck_asset(deck_asset_t id, size_t *size)
{
    switch (id) {
        case DECK_ASSET_FONT_DISPLAY:
            *size = (size_t)(orbitron_ttf_end - orbitron_ttf_start);
            return orbitron_ttf_start;
        case DECK_ASSET_FONT_MONO:
            *size = (size_t)(mono_ttf_end - mono_ttf_start);
            return mono_ttf_start;
        case DECK_ASSET_FONT_ICONS:
            *size = (size_t)(icons_ttf_end - icons_ttf_start);
            return icons_ttf_start;
        case DECK_ASSET_FONT_TERM:
            *size = (size_t)(term_ttf_end - term_ttf_start);
            return term_ttf_start;
    }
    *size = 0;
    return NULL;
}
