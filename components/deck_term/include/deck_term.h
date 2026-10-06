/*
 * deck_term: an xterm-compatible terminal widget. libvterm does the
 * emulation; this draws its cell grid in LVGL (only damaged cells are
 * redrawn), keeps scrollback in PSRAM and turns Tab5 Keyboard presses into
 * the byte sequences a remote program expects.
 *
 * Data flow:  remote --deck_term_feed()--> screen
 *             keys --deck_term_key()--> output callback --> remote
 *
 * All functions run on the LVGL task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "deck_input.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct deck_term deck_term_t;

typedef struct {
    /* Bytes to send to the remote side (keystrokes, replies to queries). */
    void (*output)(const char *data, size_t len, void *user);
    /* The grid changed size; tell the remote (SSH window-change). */
    void (*resized)(int cols, int rows, void *user);
    /* The remote set a window title (may be NULL). */
    void (*title)(const char *title, void *user);
    void *user;
    int scrollback_lines;   /* 0 = default (2000) */
} deck_term_cfg_t;

/* Creates the widget filling `parent`. */
deck_term_t *deck_term_create(lv_obj_t *parent, const deck_term_cfg_t *cfg);
void         deck_term_destroy(deck_term_t *t);
lv_obj_t    *deck_term_obj(deck_term_t *t);

void deck_term_feed(deck_term_t *t, const char *data, size_t len);

/* Translate and send a key press. Returns true if it was sent. */
bool deck_term_key(deck_term_t *t, const deck_key_t *key);
/* Send a key by libvterm code (touch key bar): VTermKey / modifiers. */
void deck_term_send_vkey(deck_term_t *t, int vterm_key, int vterm_mods);
void deck_term_send_text(deck_term_t *t, const char *utf8);

void deck_term_size(deck_term_t *t, int *cols, int *rows);

/* Scrollback view: positive scrolls back, 0 returns to live. */
void deck_term_scroll(deck_term_t *t, int lines);
void deck_term_scroll_live(deck_term_t *t);

/* Sticky Ctrl from the touch bar: applies to the next typed character. */
void deck_term_set_sticky_ctrl(deck_term_t *t, bool on);
bool deck_term_sticky_ctrl(deck_term_t *t);

#ifdef __cplusplus
}
#endif
