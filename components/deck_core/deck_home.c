#include "deck_home.h"

static deck_group_t s_groups[DECK_HOME_TILES];
static size_t s_ngroups;
static deck_tile_t s_tiles[DECK_HOME_TILES];
static size_t s_ntiles;

void deck_home_add_app(deck_app_t *app)
{
    if (app == NULL || s_ntiles >= DECK_HOME_TILES) {
        LV_LOG_ERROR("cannot add home tile");
        return;
    }
    s_tiles[s_ntiles++] = (deck_tile_t){.app = app};
}

deck_group_t *deck_home_add_group(const char *name, const char *tagline, const char *icon)
{
    if (s_ntiles >= DECK_HOME_TILES || s_ngroups >= DECK_HOME_TILES) {
        LV_LOG_ERROR("cannot add home group");
        return NULL;
    }
    deck_group_t *g = &s_groups[s_ngroups++];
    g->name          = name;
    g->tagline       = tagline;
    g->icon          = icon;
    g->count         = 0;
    s_tiles[s_ntiles++] = (deck_tile_t){.group = g};
    return g;
}

void deck_group_add(deck_group_t *group, deck_app_t *app)
{
    if (group == NULL || app == NULL || group->count >= DECK_GROUP_APPS) {
        LV_LOG_ERROR("cannot add module to group");
        return;
    }
    group->apps[group->count++] = app;
}

size_t deck_home_count(void) { return s_ntiles; }

const deck_tile_t *deck_home_tile(size_t index) { return index < s_ntiles ? &s_tiles[index] : NULL; }

deck_group_t *deck_app_group(const deck_app_t *app)
{
    for (size_t g = 0; g < s_ngroups; g++) {
        for (size_t i = 0; i < s_groups[g].count; i++) {
            if (s_groups[g].apps[i] == app) return &s_groups[g];
        }
    }
    return NULL;
}

int deck_home_index(const deck_app_t *app, const deck_group_t *group)
{
    for (size_t i = 0; i < s_ntiles; i++) {
        if ((app && s_tiles[i].app == app) || (group && s_tiles[i].group == group)) return (int)i;
    }
    return -1;
}
