/*
 * A DECK//OS app ("module") is a screen plus a few callbacks.
 *
 *   deck_app_register()     once at boot, in launcher order (tile 01, 02, ...)
 *   on_start(parent)        build the UI as children of `parent`
 *   on_key(key)             optional: see every key press first; return true
 *                           to consume it (shortcuts, terminal input)
 *   on_stop()               optional: free non-LVGL resources (timers are
 *                           fine to leave if they are attached to objects)
 *
 * The shell deletes `parent` after on_stop(), so LVGL children are freed
 * automatically. All callbacks run on the LVGL task with the lock held.
 *
 * Esc returns home unless on_key consumes it; Alt+Esc always returns home.
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

    bool (*on_start)(deck_app_t *self, lv_obj_t *parent);
    void (*on_stop)(deck_app_t *self);
    bool (*on_key)(deck_app_t *self, const deck_key_t *key);

    void *user;
};

void        deck_app_register(deck_app_t *app);
size_t      deck_app_count(void);
deck_app_t *deck_app_get(size_t index);
int         deck_app_index(const deck_app_t *app);

#ifdef __cplusplus
}
#endif
