/*
 * Visual effects. All of them are built to be cheap with LVGL direct mode,
 * where only invalidated areas are redrawn: they touch small regions or run
 * for a fraction of a second.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*deck_fx_done_cb_t)(void *user);

/* Flash glitch bars over the screen for `ms`, call `mid` halfway through
 * (swap screens there so the change is hidden by the glitch), then `done`.
 * Either callback may be NULL. */
void deck_fx_glitch(uint32_t ms, deck_fx_done_cb_t mid, deck_fx_done_cb_t done, void *user);

/* Faint CRT scanlines over everything (on the top layer). */
void deck_fx_scanlines(bool on);

/* Reveal `text` into `label` one character at a time, `cps` chars/second,
 * with a blinking block cursor that stays at the end. The animation stops
 * automatically when the label is deleted. */
void deck_fx_typewriter(lv_obj_t *label, const char *text, uint32_t cps);

#ifdef __cplusplus
}
#endif
