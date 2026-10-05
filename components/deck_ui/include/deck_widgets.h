/*
 * DECK//OS widget kit: chamfered neon panels and the small helpers every
 * screen uses. LVGL only knows rounded corners, so panels draw their own
 * cut-corner outline in an LV_EVENT_DRAW_MAIN handler.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which corners are cut at 45 degrees. */
#define DECK_CUT_TL  0x01
#define DECK_CUT_TR  0x02
#define DECK_CUT_BR  0x04
#define DECK_CUT_BL  0x08
#define DECK_CUT_DIAG (DECK_CUT_TL | DECK_CUT_BR)

/* A chamfered container. Not clickable unless you make it so; if clickable,
 * it lights up in the accent color when pressed or keyboard-focused. */
lv_obj_t *deck_panel(lv_obj_t *parent, uint8_t cuts, int32_t cut_px);

void deck_panel_set_outline(lv_obj_t *panel, lv_color_t color);
void deck_panel_set_fill(lv_obj_t *panel, lv_color_t color);
/* Short secondary-neon bar along the top edge, a "this is a live module" mark. */
void deck_panel_set_tab(lv_obj_t *panel, bool on);
/* Diagonal hazard stripes along the bottom edge (used for offline modules). */
void deck_panel_set_hazard(lv_obj_t *panel, bool on);

/* A clickable, focusable chamfered button with a centered label. */
lv_obj_t *deck_button(lv_obj_t *parent, const char *text, const lv_font_t *font);

/* An icon glyph (MDI) followed by text, as one row. The icon is sized to
 * match `font`. Update both parts later with deck_icon_text_set(). */
lv_obj_t *deck_icon_text(lv_obj_t *parent, const char *icon, const char *text, const lv_font_t *font,
                         lv_color_t color);
void deck_icon_text_set(lv_obj_t *row, const char *icon, const char *text, lv_color_t color);

/* Plain label with font and color in one call. */
lv_obj_t *deck_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);

/* Transparent layout box sized to its content. */
lv_obj_t *deck_box(lv_obj_t *parent);

/* "// TITLE ------" heading row that fills the parent's width. */
lv_obj_t *deck_section(lv_obj_t *parent, const char *title);

/* Small outlined tag, e.g. ONLINE / v0.5. */
lv_obj_t *deck_chip(lv_obj_t *parent, const char *text, lv_color_t color);

/* Draw the faint HUD grid behind `obj`'s children. */
void deck_grid_bg(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
