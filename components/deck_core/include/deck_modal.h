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
/* Same, with the text masked (passwords). */
void deck_modal_password(const char *title, deck_prompt_cb_t cb, void *user);

/* Pick one of `count` rows; `index` is -1 when cancelled. Items are copied. */
typedef void (*deck_list_cb_t)(int index, void *user);
void deck_modal_list(const char *title, const char *const *items, int count, deck_list_cb_t cb, void *user);
void deck_modal_confirm(const char *title, const char *message, const char *yes_label, deck_confirm_cb_t cb,
                        void *user);
/* Three-way question: `later_label` (also Esc) gives -1, `alt_label` 0,
 * `ok_label` 1. The first button has focus and Enter waits a second, since
 * these can appear unprompted. */
typedef void (*deck_choice_cb_t)(int choice, void *user);
void deck_modal_choice(const char *title, const char *message, const char *later_label, const char *alt_label,
                       const char *ok_label, deck_choice_cb_t cb, void *user);

/* Read-only text in `count` side-by-side columns with a CLOSE button. */
void deck_modal_info(const char *title, const char *const *columns, int count);
/* Same, for a risky "yes": CANCEL has focus and Enter does nothing for the
 * first second, so type-ahead cannot accept it. */
void deck_modal_confirm_danger(const char *title, const char *message, const char *yes_label, deck_confirm_cb_t cb,
                               void *user);

bool deck_modal_active(void);

/* Enter while a modal is open: submit the prompt, pick the focused row, or
 * press the focused button. The shell routes Enter here instead of to LVGL,
 * because LVGL acts on Enter at key-down and would deliver the key-up to
 * whatever is focused after the dialog closes. */
void deck_modal_enter(void);

/* Close like Esc: the callback gets a cancel. */
void deck_modal_cancel(void);

/* Close without calling back (the owner is going away). */
void deck_modal_discard(void);

#ifdef __cplusplus
}
#endif
