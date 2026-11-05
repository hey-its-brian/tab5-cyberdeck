/*
 * A DECK//OS app ("module") is a screen plus a few callbacks.
 *
 *   deck_app_register()     once at boot, in launcher order (tile 01, 02, ...)
 *   on_start(parent)        build the UI as children of `parent`
 *   on_key(key)             optional: see every key press first; return true
 *                           to consume it (shortcuts, terminal input)
 *   on_exit()               optional: runs first when leaving, while the
 *                           module's widgets still exist (save work here)
 *   on_stop()               optional: free non-LVGL resources; runs after
 *                           the widgets are deleted
 *
 * The shell deletes everything under `parent` between on_exit() and
 * on_stop(), so LVGL children never need freeing by hand. All callbacks run on the LVGL task with the lock held.
 *
 * Esc goes back a level unless on_key consumes it; Alt+Esc always returns
 * home. Where a module sits on the home screen is decided by deck_home.h.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "deck_input.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct deck_app deck_app_t;

struct deck_app {
    const char *name;      /* tile title, e.g. "TERMINAL" */
    const char *tagline;   /* tile subtitle, e.g. "SSH UPLINK" */
    const char *icon;      /* glyph from deck_icons.h */
    const char *eta;       /* NULL if the module works; else the version it
                              is scheduled for ("v0.5"), shown as OFFLINE */
    uint8_t hotkey;        /* HID usage: Alt+<key> opens it from anywhere; 0 = none */
    const char *id;        /* short lower-case slug, at most 11 chars ("weather");
                              its enabled state is saved as "app_<id>" */
    const char *desc;      /* one line for SETTINGS > APPLICATIONS */
    bool optional;         /* false: core app, always on */
    bool default_on;       /* optional apps: enabled until the user says otherwise */

    bool (*on_start)(deck_app_t *self, lv_obj_t *parent);
    void (*on_exit)(deck_app_t *self);
    void (*on_stop)(deck_app_t *self);
    bool (*on_key)(deck_app_t *self, const deck_key_t *key);
    /* Optional apps: start and stop background work (timers, tasks,
     * connections). on_disable may come before any on_enable, and on_enable
     * may be called twice; both must cope. */
    void (*on_enable)(deck_app_t *self);
    void (*on_disable)(deck_app_t *self);
    /* Optional: a live section in the home panel. `section` is deleted with
     * the home screen, so hang timers off it. */
    void (*on_panel)(deck_app_t *self, lv_obj_t *section);

    void *user;
};

void        deck_app_register(deck_app_t *app);
size_t      deck_app_count(void);
deck_app_t *deck_app_get(size_t index);
int         deck_app_index(const deck_app_t *app);
/* Enabled apps only: a disabled app's Alt+letter does nothing. */
deck_app_t *deck_app_by_hotkey(uint8_t code);
deck_app_t *deck_app_by_id(const char *id);

/* Core apps are always enabled. Optional apps read "app_<id>" from settings,
 * falling back to default_on. */
bool deck_app_enabled(const deck_app_t *app);
/* Saves the state and runs on_enable / on_disable when it changes. Returns
 * false (and changes nothing) for a core app or when the setting did not
 * stick. */
bool deck_app_set_enabled(deck_app_t *app, bool on);
/* Once at boot, after every app is registered: on_enable for each enabled
 * optional app. */
void deck_app_boot(void);

#ifdef __cplusplus
}
#endif
