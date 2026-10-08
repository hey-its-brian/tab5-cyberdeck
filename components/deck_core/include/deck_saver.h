/* Screen timeout: after SYSTEM > SAVER minutes without a key or touch, a
 * drifting clock covers the screen with the backlight dimmed; ten minutes
 * later the screen sleeps fully. Any key or touch wakes it, and that press
 * is swallowed. */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call about once a second (the status bar does). */
void deck_saver_tick(void);

bool deck_saver_active(void);
void deck_saver_wake(void);

/* Activity that is not a key or touch but should keep the screen on
 * (terminal output, a portal transfer). */
void deck_saver_poke(void);

/* Timeout in minutes, 0 = off. Saved in settings ("saver"). */
int deck_saver_timeout(void);
void deck_saver_set_timeout(int minutes);

#ifdef __cplusplus
}
#endif
