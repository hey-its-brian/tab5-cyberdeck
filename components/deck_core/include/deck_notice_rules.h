/*
 * The rules behind notices (deck_notice.h), with no LVGL in them so they
 * can be tested on the host: what a level does to the unseen count, when a
 * banner is shown, when one replaces another, and when an ALERT wakes a
 * dark screen.
 *
 *   INFO   accepted, shown nowhere.
 *   WARN   counts; a banner for a few seconds, unless the app that raised
 *          it is on screen, the screen is dark, another banner went up in
 *          the last few seconds, or an ALERT banner is up.
 *   ALERT  counts (twice: it is also an unseen alert); a banner unless the
 *          app that raised it is on a lit screen; it replaces whatever is
 *          up, ignores the quiet period, stays longer, and wakes a dark
 *          screen (the app being in front does not stop that: nobody is
 *          looking at a dark screen).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DECK_NOTICE_INFO, DECK_NOTICE_WARN, DECK_NOTICE_ALERT } deck_notice_level_t;

#define DECK_NOTICE_BANNER_MS       4000  /* how long a warning's banner stays */
#define DECK_NOTICE_ALERT_BANNER_MS 20000 /* and an alert's */
#define DECK_NOTICE_QUIET_MS        10000 /* at most one new warning banner in this long */

typedef struct {
    bool                banner_up;
    deck_notice_level_t banner_level;
    uint32_t            shown_at; /* when the current or the last banner went up */
    bool                shown_once;
    unsigned            unseen, unseen_alerts;
} deck_notice_rules_t;

typedef enum { DECK_NOTICE_NONE, DECK_NOTICE_BANNER, DECK_NOTICE_BANNER_WAKE } deck_notice_act_t;

/* Count the notice and say what to do with it. BANNER: show one (it
 * replaces what is up). BANNER_WAKE: wake the screen first. */
deck_notice_act_t deck_notice_rules_post(deck_notice_rules_t *r, deck_notice_level_t level, bool app_on_screen, bool screen_dark,
                                         uint32_t now_ms);
void     deck_notice_rules_banner_gone(deck_notice_rules_t *r); /* the banner closed or timed out */
void     deck_notice_rules_seen(deck_notice_rules_t *r);        /* the warnings have been shown */
uint32_t deck_notice_rules_banner_ms(deck_notice_level_t level);

#ifdef __cplusplus
}
#endif
