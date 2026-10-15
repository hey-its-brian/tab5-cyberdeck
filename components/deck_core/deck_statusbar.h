/* Internal to deck_core: the HUD status bar owned by the shell. */
#pragma once

#include "deck_app.h"
#include "deck_home.h"

void deck_statusbar_create(lv_obj_t *parent);
/* Where we are: both NULL = home, a group alone = its screen, an app with
 * the group it sits in (or NULL for a home tile) = that app. */
void deck_statusbar_set_path(deck_group_t *group, deck_app_t *app);
void deck_statusbar_destroy(void);
