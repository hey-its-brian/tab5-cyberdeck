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

    bool (*on_start)(deck_app_t *self, lv_obj_t *parent);
    void (*on_exit)(deck_app_t *self);
    void (*on_stop)(deck_app_t *self);
    bool (*on_key)(deck_app_t *self, const deck_key_t *key);
    /* Optional: the home tile shows live content (a widget) instead of the
     * icon and name. `body` is the tile's inner box; it is deleted with the
     * home screen, so hang timers and handles off it. */
    void (*on_tile)(deck_app_t *self, lv_obj_t *body);

    void *user;
};

void        deck_app_register(deck_app_t *app);
size_t      deck_app_count(void);
deck_app_t *deck_app_get(size_t index);
int         deck_app_index(const deck_app_t *app);
deck_app_t *deck_app_by_hotkey(uint8_t code);

#ifdef __cplusplus
}
#endif
