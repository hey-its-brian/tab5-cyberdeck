/*
 * Binary assets (fonts). On the device they are linked into flash with
 * EMBED_FILES; the simulator reads them from assets/ at startup.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DECK_ASSET_FONT_DISPLAY,   /* Orbitron */
    DECK_ASSET_FONT_MONO,      /* Share Tech Mono */
    DECK_ASSET_FONT_ICONS,     /* Material Design Icons subset */
} deck_asset_t;

/* Returns a pointer to the asset bytes (valid for the program lifetime) and
 * stores its size in *size, or NULL if the asset is missing. */
const uint8_t *deck_asset(deck_asset_t id, size_t *size);

#ifdef __cplusplus
}
#endif
