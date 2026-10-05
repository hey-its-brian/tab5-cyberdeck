/* Internal to deck_core: the HUD status bar owned by the shell. */
#pragma once

#include "deck_app.h"

void deck_statusbar_create(lv_obj_t *parent);
void deck_statusbar_set_app(deck_app_t *app);   /* NULL = home */
void deck_statusbar_destroy(void);
