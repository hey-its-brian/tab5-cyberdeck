/* Internal to deck_core: the home screen and the group screens. */
#pragma once

#include "deck_home.h"
#include "deck_input.h"

/* `group` NULL builds the home screen; otherwise that group's tiles. */
void deck_launcher_create(lv_obj_t *parent, deck_group_t *group);
bool deck_launcher_key(const deck_key_t *key);
/* Home tile style from settings ("tile_style"), shared with the live panel. */
enum { STYLE_GLOW, STYLE_TWO_TONE, STYLE_NEON, STYLE_COUNT };
int deck_tile_style(void);
/* Called for a tap or Enter on a greyed (switched-off) APPS tile, with that
 * app: SETTINGS registers a function that opens APPLICATIONS on it. */
void deck_launcher_set_manage(void (*fn)(const deck_app_t *focus));
