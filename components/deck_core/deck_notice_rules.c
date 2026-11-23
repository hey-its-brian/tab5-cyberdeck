/* The notice rules. See deck_notice_rules.h. */
#include "deck_notice_rules.h"

uint32_t deck_notice_rules_banner_ms(deck_notice_level_t level) { return level == DECK_NOTICE_ALERT ? DECK_NOTICE_ALERT_BANNER_MS : DECK_NOTICE_BANNER_MS; }

deck_notice_act_t deck_notice_rules_post(deck_notice_rules_t *r, deck_notice_level_t level, bool app_on_screen, bool screen_dark, uint32_t now_ms)
{
    if (level == DECK_NOTICE_INFO) return DECK_NOTICE_NONE;
    r->unseen++;
    if (level == DECK_NOTICE_ALERT) r->unseen_alerts++;
    /* They are looking at it: nothing, unless the screen is dark, in which case nobody is. */
    if (app_on_screen && (!screen_dark || level == DECK_NOTICE_WARN)) return DECK_NOTICE_NONE;
    if (level == DECK_NOTICE_WARN) {
        if (screen_dark) return DECK_NOTICE_NONE;
        if (r->banner_up && r->banner_level == DECK_NOTICE_ALERT) return DECK_NOTICE_NONE;
        if (r->shown_once && (uint32_t)(now_ms - r->shown_at) < DECK_NOTICE_QUIET_MS) return DECK_NOTICE_NONE;
    }
    r->banner_up    = true;
    r->banner_level = level;
    r->shown_at     = now_ms;
    r->shown_once   = true;
    return level == DECK_NOTICE_ALERT && screen_dark ? DECK_NOTICE_BANNER_WAKE : DECK_NOTICE_BANNER;
}

void deck_notice_rules_banner_gone(deck_notice_rules_t *r) { r->banner_up = false; }

void deck_notice_rules_seen(deck_notice_rules_t *r)
{
    r->unseen = r->unseen_alerts = 0;
    r->banner_up                 = false;
}
