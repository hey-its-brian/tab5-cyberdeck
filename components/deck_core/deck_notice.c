/* Notices: the unseen count, the banner and the status bar flag. See deck_notice.h. */
#include "deck_notice.h"

#include <stdio.h>
#include <string.h>

#include "deck_icons.h"
#include "deck_saver.h"
#include "deck_shell.h"
#include "deck_statusbar.h"
#include "deck_theme.h"
#include "deck_widgets.h"

static deck_notice_rules_t s_rules;
static deck_app_t         *s_app;
static char                s_flag[16], s_latest[64];
static lv_obj_t           *s_banner;
static lv_timer_t         *s_hide;

/* The widgets only: a new banner replacing an old one is not the banner going. */
static void banner_widgets_close(void)
{
    if (s_hide) lv_timer_delete(s_hide);
    s_hide = NULL;
    if (s_banner) lv_obj_delete(s_banner);
    s_banner = NULL;
}

static void banner_close(void)
{
    banner_widgets_close();
    deck_notice_rules_banner_gone(&s_rules);
}

static void hide_cb(lv_timer_t *t)
{
    (void)t;
    banner_close();
}

static void banner_clicked(lv_event_t *e)
{
    (void)e;
    deck_app_t *app = s_app;
    banner_close();
    if (app && deck_app_enabled(app) && deck_shell_current() != app) deck_shell_launch(app);
}

static void banner_show(deck_notice_level_t level, const char *source, const char *text)
{
    bool alert = level == DECK_NOTICE_ALERT;
    banner_widgets_close();
    /* On the top layer so it rides over whatever app is running; it holds
     * no focus, so typing carries on underneath. */
    s_banner = deck_panel(lv_layer_top(), DECK_CUT_TL | DECK_CUT_BR, 12);
    lv_obj_set_size(s_banner, 760, LV_SIZE_CONTENT);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, DECK_STATUSBAR_H + 10);
    lv_obj_set_style_pad_hor(s_banner, 18, 0);
    lv_obj_set_style_pad_ver(s_banner, 12, 0);
    lv_obj_set_flex_flow(s_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_banner, 14, 0);
    deck_panel_set_outline(s_banner, alert ? g_pal.danger : g_pal.warn);
    deck_panel_set_fill(s_banner, g_pal.panel);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_banner, banner_clicked, LV_EVENT_CLICKED, NULL);
    lv_group_remove_obj(s_banner); /* never in the Tab order */

    char src[12];
    snprintf(src, sizeof(src), "%s", source ? source : "");
    for (char *p = src; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    /* The level is a word as well as a colour: an alert says so before its source. */
    deck_icon_text(s_banner, ICON_ALERT, alert ? "ALERT" : src, g_font.mono_m, alert ? g_pal.danger : g_pal.warn);
    lv_obj_t *msg = deck_label(s_banner, g_font.mono_m, g_pal.text, text ? text : "");
    lv_obj_set_flex_grow(msg, 1);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_MODE_DOTS);

    s_hide = lv_timer_create(hide_cb, deck_notice_rules_banner_ms(level), NULL);
}

void deck_notice_post(deck_notice_level_t level, const char *source, const char *text, deck_app_t *app)
{
    if (level == DECK_NOTICE_INFO) return;
    bool on_screen = app && deck_shell_current() == app;
    bool dark      = deck_shell_sleeping() || deck_saver_active();
    deck_notice_act_t act = deck_notice_rules_post(&s_rules, level, on_screen, dark, lv_tick_get());
    s_app = app;
    snprintf(s_latest, sizeof(s_latest), "%s", text ? text : "");
    deck_statusbar_refresh();
    if (act == DECK_NOTICE_NONE) return;
    if (act == DECK_NOTICE_BANNER_WAKE) {
        deck_saver_wake();
        deck_shell_wake();
    }
    banner_show(level, source, text);
}

unsigned deck_notice_unseen(void) { return s_rules.unseen; }

unsigned deck_notice_unseen_alerts(void) { return s_rules.unseen_alerts; }

const char *deck_notice_latest(void) { return s_latest; }

void deck_notice_seen(void)
{
    if (s_rules.unseen) deck_statusbar_refresh();
    deck_notice_rules_seen(&s_rules);
    s_latest[0] = 0;
    banner_close();
}

deck_app_t *deck_notice_app(void) { return s_app; }

void deck_notice_set_flag(const char *text)
{
    snprintf(s_flag, sizeof(s_flag), "%s", text ? text : "");
    deck_statusbar_refresh();
}

const char *deck_notice_flag(void) { return s_flag[0] ? s_flag : NULL; }
