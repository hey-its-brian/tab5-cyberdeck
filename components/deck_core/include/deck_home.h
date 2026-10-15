/*
 * The home screen layout: a row of tiles, each a module or a group of
 * modules. A group opens a screen of its own with the same kind of tiles.
 *
 * Modules are still registered flat with deck_app_register() (that is what
 * Alt+letter shortcuts and name lookups use); this only decides where each
 * one shows up. Set it up once at boot, after the modules are registered.
 */
#pragma once

#include "deck_app.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_HOME_TILES 6
#define DECK_GROUP_APPS 6

typedef struct {
    const char *name;    /* tile title, e.g. "TOOLS" */
    const char *tagline; /* tile subtitle */
    const char *icon;    /* glyph from deck_icons.h */
    deck_app_t *apps[DECK_GROUP_APPS];
    size_t count;
} deck_group_t;

/* One home tile: exactly one of the two is set. */
typedef struct {
    deck_app_t *app;
    deck_group_t *group;
} deck_tile_t;

void          deck_home_add_app(deck_app_t *app);
deck_group_t *deck_home_add_group(const char *name, const char *tagline, const char *icon);
void          deck_group_add(deck_group_t *group, deck_app_t *app);

size_t             deck_home_count(void);
const deck_tile_t *deck_home_tile(size_t index);

/* The group a module sits in, or NULL when it is a home tile itself. */
deck_group_t *deck_app_group(const deck_app_t *app);

/* Home tile position (0-based) of a module or a group, -1 if not on it. */
int deck_home_index(const deck_app_t *app, const deck_group_t *group);

#ifdef __cplusplus
}
#endif
