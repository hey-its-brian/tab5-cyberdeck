/* Internal to deck_core: the home screen and the group screens. */
#pragma once

#include "deck_home.h"
#include "deck_input.h"

/* `group` NULL builds the home screen; otherwise that group's tiles. */
void deck_launcher_create(lv_obj_t *parent, deck_group_t *group);
bool deck_launcher_key(const deck_key_t *key);
