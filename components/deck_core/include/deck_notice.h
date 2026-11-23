/*
 * Notices: how an app tells the user something while they are somewhere
 * else. A warning adds to an unseen count that the status bar shows until
 * the app says it has been seen, and drops a banner under the status bar
 * for a few seconds. An alert does the same in red, stays longer, replaces
 * a warning's banner and wakes a dark screen (the rules are in
 * deck_notice_rules.h). Nothing takes keyboard focus and nothing makes a
 * sound.
 *
 * An app can also raise a flag: a short word the status bar shows in the
 * danger color for as long as it is set ("RED TEAM").
 *
 * LVGL task only. A task that notices something records it and lets a timer
 * on the LVGL task post it (RF WATCH does this from its background timer).
 */
#pragma once

#include "deck_app.h"
#include "deck_notice_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `app` is what a tap on the banner or the status bar count opens; NULL for
 * nothing. Info notices are accepted and, for now, shown nowhere. A warning
 * or an alert always counts; what its banner does is deck_notice_rules.h. */
void deck_notice_post(deck_notice_level_t level, const char *source, const char *text, deck_app_t *app);

unsigned    deck_notice_unseen(void);        /* warnings and alerts since the last deck_notice_seen() */
unsigned    deck_notice_unseen_alerts(void); /* the alerts among them */
const char *deck_notice_latest(void);        /* the newest one's text, "" when none */
void        deck_notice_seen(void);
deck_app_t *deck_notice_app(void);    /* the app of the latest warning, or NULL */

void        deck_notice_set_flag(const char *text); /* NULL clears */
const char *deck_notice_flag(void);                 /* NULL when none */

#ifdef __cplusplus
}
#endif
