#include "deck_live.h"

#include <stdio.h>
#include <time.h>

#include "deck_app.h"
#include "deck_launcher.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define LIVE_MAX 2 /* app sections under the clock */
#define LIVE_PAD 18
#define LIVE_GAP 14

/* ---- Clock: the panel's headline; tap it to sleep the screen ------------- */

static lv_obj_t *s_hm, *s_ss, *s_date;
static lv_timer_t *s_clock_timer;

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    static const char *days[]   = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
    static const char *months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_hm, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_label_set_text_fmt(s_ss, "%02d", tm.tm_sec);
    lv_label_set_text_fmt(s_date, "%s %02d %s %04d", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
                          tm.tm_year + 1900);
}

static void clock_deleted(lv_event_t *e)
{
    (void)e;
    if (s_clock_timer) lv_timer_delete(s_clock_timer);
    s_clock_timer = NULL;
    s_hm = s_ss = s_date = NULL;
}

static void clock_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_sleep();
}

static void build_clock(lv_obj_t *panel)
{
    lv_obj_t *box = deck_box(panel);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 2, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(box, clock_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(box, clock_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *row = deck_box(box);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_color_t big = deck_tile_style() == STYLE_TWO_TONE ? g_pal.accent2 : g_pal.text;
    s_hm = deck_label(row, g_font.disp_xl, big, "--:--");
    s_ss = deck_label(row, g_font.disp_m, g_pal.dim, "--");
    lv_obj_set_style_pad_bottom(s_ss, 8, 0);
    s_date = deck_label(box, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_style_text_letter_space(s_date, 1, 0);

    clock_tick(NULL);
    s_clock_timer = lv_timer_create(clock_tick, 1000, NULL);
}

/* ---- Panel --------------------------------------------------------------- */

static void section_clicked(lv_event_t *e)
{
    deck_shell_launch((deck_app_t *)lv_event_get_user_data(e));
}

static void style_panel(lv_obj_t *panel)
{
    switch (deck_tile_style()) {
        case STYLE_TWO_TONE:
            deck_panel_set_fill(panel, g_pal.panel_hi);
            deck_panel_set_outline(panel, lv_color_mix(g_pal.accent2, g_pal.line, 110));
            break;
        case STYLE_NEON:
            deck_panel_set_wash(panel, true, g_pal.accent);
            deck_panel_set_outline(panel, lv_color_mix(g_pal.accent, g_pal.line, 110));
            break;
        default:
            deck_panel_set_wash(panel, true, g_pal.accent2);
            deck_panel_set_outline(panel, lv_color_mix(g_pal.accent2, g_pal.line, 110));
            break;
    }
}

lv_obj_t *deck_live_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *panel = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(panel, w, h);
    deck_panel_set_tab(panel, true);
    style_panel(panel);
    lv_obj_set_style_pad_all(panel, LIVE_PAD, 0);
    lv_obj_set_style_pad_top(panel, LIVE_PAD + 4, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, LIVE_GAP, 0);

    build_clock(panel);
    lv_obj_t *rule = deck_box(panel);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(rule, g_pal.line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);

    int shown = 0;
    for (size_t i = 0; i < deck_app_count() && shown < LIVE_MAX; i++) {
        deck_app_t *a = deck_app_get(i);
        if (!a->optional || !a->on_panel || a->eta || !deck_app_enabled(a)) continue;

        lv_obj_t *sec = deck_box(panel);
        lv_obj_set_width(sec, LV_PCT(100));
        lv_obj_set_flex_grow(sec, 1);
        lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(sec, 4, 0);
        lv_obj_add_flag(sec, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sec, section_clicked, LV_EVENT_CLICKED, a);
        deck_label(sec, g_font.mono_s, g_pal.accent, a->name);

        lv_obj_t *body = deck_box(sec);
        lv_obj_set_width(body, LV_PCT(100));
        lv_obj_set_flex_grow(body, 1);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE); /* taps reach the section */
        a->on_panel(a, body);
        shown++;
    }

    if (shown == 0) {
        /* Nothing live switched on: a quiet mark fills the space. */
        lv_obj_t *fill = deck_box(panel);
        lv_obj_set_width(fill, LV_PCT(100));
        lv_obj_set_flex_grow(fill, 1);
        lv_obj_t *mark = deck_label(fill, g_font.disp_m, g_pal.line, "DECK//OS");
        lv_obj_center(mark);
    }
    return panel;
}
