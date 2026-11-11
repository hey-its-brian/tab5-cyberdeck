/*
 * Notices: how an app tells the user something while they are somewhere
 * else. A warning adds to an unseen count that the status bar shows until
 * the app says it has been seen, and drops a banner under the status bar
 * for a few seconds. Nothing takes keyboard focus and nothing makes a sound.
 *
 * An app can also raise a flag: a short word the status bar shows in the
 * danger color for as long as it is set ("RED TEAM").
 *
 * LVGL task only. A task that notices something records it and lets a timer
 * on the LVGL task post it (RF WATCH does this from its background timer).
 */
#pragma once

#include "deck_app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DECK_NOTICE_INFO, DECK_NOTICE_WARN } deck_notice_level_t;

#define DECK_NOTICE_BANNER_MS 4000  /* how long a banner stays */
#define DECK_NOTICE_QUIET_MS  10000 /* at most one new banner in this long */

/* `app` is what a tap on the banner or the status bar count opens; NULL for
 * nothing. Info notices are accepted and, for now, shown nowhere. A warning
 * always counts; its banner is skipped while `app` is on screen, the screen
 * sleeps, the saver runs, or another banner showed too recently. */
void deck_notice_post(deck_notice_level_t level, const char *source, const char *text, deck_app_t *app);

unsigned    deck_notice_unseen(void); /* warnings since the last deck_notice_seen() */
void        deck_notice_seen(void);
deck_app_t *deck_notice_app(void);    /* the app of the latest warning, or NULL */

void        deck_notice_set_flag(const char *text); /* NULL clears */
const char *deck_notice_flag(void);                 /* NULL when none */

#ifdef __cplusplus
}
#endif
