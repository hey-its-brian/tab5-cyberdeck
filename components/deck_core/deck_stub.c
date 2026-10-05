/*
 * "Module offline" screen for apps that are scheduled but not written yet.
 * Keeps the launcher honest while still looking like part of the deck.
 */
#include "deck_shell.h"

#include "deck_fx.h"
#include "deck_theme.h"
#include "deck_widgets.h"

static void return_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_home();
}

bool deck_stub_screen(deck_app_t *app, lv_obj_t *parent, const char *body)
{
    lv_obj_t *p = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 28);
    lv_obj_set_size(p, 1040, 540);
    lv_obj_center(p);
    deck_panel_set_tab(p, true);
    deck_panel_set_hazard(p, true);
    lv_obj_set_style_pad_all(p, 36, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 18, 0);

    lv_obj_t *head = deck_box(p);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 18, 0);
    deck_label(head, g_font.icon_m, g_pal.accent, app->icon);
    lv_obj_t *title = deck_label(head, g_font.disp_l, g_pal.text, "");
    lv_label_set_text_fmt(title, "MODULE // %s", app->name);
    lv_obj_set_flex_grow(title, 1);
    deck_chip(head, "OFFLINE", g_pal.danger);
    if (app->eta) {
        lv_obj_t *c = deck_chip(head, "", g_pal.warn);
        lv_label_set_text_fmt(lv_obj_get_child(c, 0), "SCHEDULED %s", app->eta);
    }

    deck_section(p, "MANIFEST");

    lv_obj_t *text = deck_label(p, g_font.mono_m, g_pal.text, "");
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_text_line_space(text, 6, 0);
    deck_fx_typewriter(text, body, 140);

    lv_obj_t *foot = deck_box(p);
    lv_obj_set_width(foot, LV_PCT(100));
    lv_obj_t *hint = deck_label(foot, g_font.mono_s, g_pal.dim, "[ESC] RETURN TO DECK    [ALT+1..5] SWITCH MODULE");
    lv_obj_align(hint, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *btn = deck_button(foot, "RETURN", NULL);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btn, return_clicked, LV_EVENT_CLICKED, NULL);
    return true;
}
