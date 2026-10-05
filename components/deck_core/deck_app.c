#include "deck_app.h"

#define APP_MAX 9 /* Alt+1..9 */

static deck_app_t *s_apps[APP_MAX];
static size_t s_count;

void deck_app_register(deck_app_t *app)
{
    if (app == NULL || app->on_start == NULL || s_count >= APP_MAX) {
        LV_LOG_ERROR("cannot register app");
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
