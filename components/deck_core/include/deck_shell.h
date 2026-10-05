/*
 * The shell owns the screen: status bar on top, launcher or the running app
 * below, transitions between them, and the boot sequence.
 */
#pragma once

#include <stdbool.h>

#include "deck_app.h"
#include "deck_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DECK_VERSION "0.4.0"

#define DECK_STATUSBAR_H 56

/* Build the shell. With `boot_anim`, play the POST sequence first. */
void deck_shell_start(bool boot_anim);

void deck_shell_home(void);
void deck_shell_launch(deck_app_t *app);
deck_app_t *deck_shell_current(void);

/* Tear down and rebuild everything visible (after an accent change). The
 * running app is restarted in place, without a transition. */
void deck_shell_rebuild(void);

/* Called by the input pump for every key press. Returns true if the shell
 * (or the app's on_key) consumed it. */
bool deck_shell_key(const deck_key_t *key);

/* Built-in "module offline" screen used by apps that are not written yet.
 * `body` is typed out line by line. */
bool deck_stub_screen(deck_app_t *app, lv_obj_t *parent, const char *body);

#ifdef __cplusplus
}
#endif
