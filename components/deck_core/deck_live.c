#include "deck_live.h"

#include "deck_app.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define LIVE_MAX 3
#define LIVE_PAD 18
#define LIVE_GAP 14

static void section_clicked(lv_event_t *e)
{
    deck_shell_launch((deck_app_t *)lv_event_get_user_data(e));
}

lv_obj_t *deck_live_create(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *panel = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_pad_all(panel, LIVE_PAD, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, LIVE_GAP, 0);

    int32_t sec_h = (h - 2 * LIVE_PAD - (LIVE_MAX - 1) * LIVE_GAP) / LIVE_MAX;
    int shown     = 0;
    for (size_t i = 0; i < deck_app_count() && shown < LIVE_MAX; i++) {
        deck_app_t *a = deck_app_get(i);
        if (!a->optional || !a->on_panel || a->eta || !deck_app_enabled(a)) continue;

        lv_obj_t *sec = deck_box(panel);
        lv_obj_set_size(sec, LV_PCT(100), sec_h);
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
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        deck_label(panel, g_font.disp_m, g_pal.line, "DECK//OS");
    }
    return panel;
}
