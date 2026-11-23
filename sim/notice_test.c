/* Host unit tests for the notice rules (deck_notice_rules.c). Run: sim/build/notice_test */
#include <stdio.h>
#include <string.h>

#include "deck_notice_rules.h"

static int s_fail, s_count;

static void expect(bool cond, const char *what)
{
    s_count++;
    if (!cond) {
        printf("FAIL  %s\n", what);
        s_fail++;
    }
}

static deck_notice_rules_t fresh(void)
{
    deck_notice_rules_t r;
    memset(&r, 0, sizeof(r));
    return r;
}

static void info_is_nothing(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_INFO, false, false, 1000) == DECK_NOTICE_NONE, "info: no banner");
    expect(r.unseen == 0 && r.unseen_alerts == 0, "info: not counted");
}

static void warn_counts_and_banners(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 1000) == DECK_NOTICE_BANNER, "warn: a banner");
    expect(r.unseen == 1 && r.unseen_alerts == 0, "warn: counted once, not as an alert");
    expect(r.banner_up && r.banner_level == DECK_NOTICE_WARN, "warn: the banner is up at WARN");
}

static void warn_quiet_period(void)
{
    deck_notice_rules_t r = fresh();
    deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 1000);
    deck_notice_rules_banner_gone(&r);
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 6000) == DECK_NOTICE_NONE, "warn: none within the quiet period");
    expect(r.unseen == 2, "warn: still counted");
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 11001) == DECK_NOTICE_BANNER, "warn: a banner after the quiet period");
}

static void warn_dark_counts_only(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, true, 1000) == DECK_NOTICE_NONE, "warn, dark: no banner");
    expect(r.unseen == 1, "warn, dark: counted");
}

static void warn_skipped_on_screen(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, true, false, 1000) == DECK_NOTICE_NONE, "warn, app on screen: no banner");
    expect(r.unseen == 1, "warn, app on screen: counted");
}

static void alert_ignores_quiet(void)
{
    deck_notice_rules_t r = fresh();
    deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 1000);
    expect(deck_notice_rules_post(&r, DECK_NOTICE_ALERT, false, false, 2000) == DECK_NOTICE_BANNER, "alert: a banner a second after a warning's");
    expect(r.banner_level == DECK_NOTICE_ALERT, "alert: it replaced the warning's banner");
    expect(r.unseen == 2 && r.unseen_alerts == 1, "alert: counted, and as an alert");
}

static void alert_wakes_dark(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_ALERT, false, true, 1000) == DECK_NOTICE_BANNER_WAKE, "alert, dark: wake and banner");
}

static void alert_skipped_on_screen(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_ALERT, true, false, 1000) == DECK_NOTICE_NONE, "alert, app on screen: no banner");
    expect(r.unseen_alerts == 1, "alert, app on screen: counted");
}

static void alert_keeps_banner_over_warn(void)
{
    deck_notice_rules_t r = fresh();
    deck_notice_rules_post(&r, DECK_NOTICE_ALERT, false, false, 1000);
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 2000) == DECK_NOTICE_NONE, "warn under an alert banner: none");
    expect(r.unseen == 2, "warn under an alert banner: counted");
    expect(r.banner_up && r.banner_level == DECK_NOTICE_ALERT, "warn under an alert banner: the alert's banner stays");
    deck_notice_rules_banner_gone(&r);
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 30000) == DECK_NOTICE_BANNER, "warn once the alert's banner is gone");
}

static void alert_keeps_banner_over_warn_after_quiet(void)
{
    deck_notice_rules_t r = fresh();
    deck_notice_rules_post(&r, DECK_NOTICE_ALERT, false, false, 1000);
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 13000) == DECK_NOTICE_NONE, "warn 12 s under an alert banner: none");
    expect(r.banner_level == DECK_NOTICE_ALERT, "warn 12 s under an alert banner: the alert's banner stays");
}

static void alert_wakes_dark_even_with_the_app_on_screen(void)
{
    deck_notice_rules_t r = fresh();
    expect(deck_notice_rules_post(&r, DECK_NOTICE_ALERT, true, true, 1000) == DECK_NOTICE_BANNER_WAKE, "alert, app on a dark screen: wake");
    expect(deck_notice_rules_post(&r, DECK_NOTICE_WARN, true, true, 2000) == DECK_NOTICE_NONE, "warn, app on a dark screen: nothing");
}

static void banner_ms(void)
{
    expect(deck_notice_rules_banner_ms(DECK_NOTICE_WARN) == 4000, "a warning's banner stays 4 s");
    expect(deck_notice_rules_banner_ms(DECK_NOTICE_ALERT) == 20000, "an alert's banner stays 20 s");
}

static void seen_clears_both_counts(void)
{
    deck_notice_rules_t r = fresh();
    deck_notice_rules_post(&r, DECK_NOTICE_WARN, false, false, 1000);
    deck_notice_rules_post(&r, DECK_NOTICE_ALERT, false, false, 2000);
    deck_notice_rules_seen(&r);
    expect(r.unseen == 0 && r.unseen_alerts == 0, "seen: both counts cleared");
    expect(!r.banner_up, "seen: the banner is closed");
}

int main(void)
{
    info_is_nothing();
    warn_counts_and_banners();
    warn_quiet_period();
    warn_dark_counts_only();
    warn_skipped_on_screen();
    alert_ignores_quiet();
    alert_wakes_dark();
    alert_skipped_on_screen();
    alert_keeps_banner_over_warn();
    alert_keeps_banner_over_warn_after_quiet();
    alert_wakes_dark_even_with_the_app_on_screen();
    banner_ms();
    seen_clears_both_counts();
    printf("%d checks, %d failed\n", s_count, s_fail);
    return s_fail ? 1 : 0;
}
