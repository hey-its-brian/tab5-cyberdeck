/*
 * Modal dialogs on the top layer: a one-line text prompt and a yes/no
 * confirm. While open they own keyboard focus; Enter submits, Esc cancels.
 * Only one modal at a time; callbacks run after the dialog is gone, so they
 * may open another one.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `text` is NULL when cancelled. */
typedef void (*deck_prompt_cb_t)(const char *text, void *user);
typedef void (*deck_confirm_cb_t)(bool yes, void *user);

void deck_modal_prompt(const char *title, const char *initial, deck_prompt_cb_t cb, void *user);
void deck_modal_confirm(const char *title, const char *message, const char *yes_label, deck_confirm_cb_t cb,
                        void *user);

bool deck_modal_active(void);

/* Close like Esc: the callback gets a cancel. */
void deck_modal_cancel(void);

/* Close without calling back (the owner is going away). */
void deck_modal_discard(void);

#ifdef __cplusplus
}
#endif
