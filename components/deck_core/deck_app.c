#include "deck_app.h"

#include <stdio.h>
#include <string.h>

#include "deck_hal.h"

#define APP_MAX 20
#define OPTIONAL_MAX 12 /* the APPS grid tops out at 4x3 */
#define ID_MAX 11       /* NVS keys are 15 chars: "app_" + id */

static deck_app_t *s_apps[APP_MAX];
static size_t s_count;

static size_t optional_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < s_count; i++) n += s_apps[i]->optional;
    return n;
}

void deck_app_register(deck_app_t *app)
{
    if (app == NULL || app->on_start == NULL || s_count >= APP_MAX) {
        LV_LOG_ERROR("cannot register app");
        return;
    }
    if (app->optional && (app->id == NULL || strlen(app->id) > ID_MAX || optional_count() >= OPTIONAL_MAX)) {
        LV_LOG_ERROR("cannot register optional app");
        return;
    }
    s_apps[s_count++] = app;
}

size_t deck_app_count(void) { return s_count; }

deck_app_t *deck_app_get(size_t index) { return index < s_count ? s_apps[index] : NULL; }

int deck_app_index(const deck_app_t *app)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_apps[i] == app) return (int)i;
    }
    return -1;
}

deck_app_t *deck_app_by_hotkey(uint8_t code)
{
    for (size_t i = 0; i < s_count; i++) {
        if (code && s_apps[i]->hotkey == code && deck_app_enabled(s_apps[i])) return s_apps[i];
    }
    return NULL;
}

deck_app_t *deck_app_by_id(const char *id)
{
    for (size_t i = 0; id && i < s_count; i++) {
        if (s_apps[i]->id && strcmp(s_apps[i]->id, id) == 0) return s_apps[i];
    }
    return NULL;
}

static void cfg_key(const deck_app_t *app, char *key, size_t n) { snprintf(key, n, "app_%s", app->id); }

bool deck_app_enabled(const deck_app_t *app)
{
    if (app == NULL) return false;
    if (!app->optional) return true;
    char key[16];
    cfg_key(app, key, sizeof(key));
    return hal_cfg_get_i32(key, app->default_on ? 1 : 0) != 0;
}

bool deck_app_set_enabled(deck_app_t *app, bool on)
{
    if (app == NULL || !app->optional) return false;
    bool was = deck_app_enabled(app);
    char key[16];
    cfg_key(app, key, sizeof(key));
    hal_cfg_set_i32(key, on ? 1 : 0);
    /* hal_cfg_set_i32 cannot report a failed write, so read it back
     * (-1 is never written, so it means "not there"). */
    if (hal_cfg_get_i32(key, -1) != (on ? 1 : 0)) return false;
    if (was != on) {
        if (on && app->on_enable) app->on_enable(app);
        if (!on && app->on_disable) app->on_disable(app);
    }
    return true;
}

void deck_app_boot(void)
{
    for (size_t i = 0; i < s_count; i++) {
        deck_app_t *a = s_apps[i];
        if (a->optional && a->on_enable && deck_app_enabled(a)) a->on_enable(a);
    }
}
