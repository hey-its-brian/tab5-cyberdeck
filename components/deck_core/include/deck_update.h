/* Automatic update checks with an install / ignore popup, and the shared
 * install progress screen (also used by SYSTEM > UPDATE). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Call about once a second (the status bar does). */
void deck_update_tick(void);

/* Install the available update with the full-screen progress view. */
void deck_update_install(void);

#ifdef __cplusplus
}
#endif
