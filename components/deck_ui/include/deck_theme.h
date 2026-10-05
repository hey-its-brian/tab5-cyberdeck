/*
 * DECK//OS visual language: palette tokens and fonts.
 *
 * Every widget reads colors from g_pal at creation time. Changing the accent
 * updates g_pal; the shell then rebuilds the visible screen so the new colors
 * take effect everywhere.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_color_t bg;        /* screen background */
    lv_color_t grid;      /* background grid lines */
    lv_color_t panel;     /* panel fill */
    lv_color_t panel_hi;  /* pressed / raised panel fill */
    lv_color_t line;      /* idle panel outline */
    lv_color_t text;      /* primary text */
    lv_color_t dim;       /* secondary text */
    lv_color_t accent;    /* primary neon (user selectable) */
    lv_color_t accent2;   /* secondary neon that pairs with the accent */
    lv_color_t warn;      /* acid yellow */
    lv_color_t ok;        /* status green */
    lv_color_t danger;    /* alarm red */
} deck_palette_t;

extern deck_palette_t g_pal;

typedef struct {
    const lv_font_t *mono_s;    /* Share Tech Mono 16: HUD details */
    const lv_font_t *mono_m;    /* Share Tech Mono 20: body text */
    const lv_font_t *mono_l;    /* Share Tech Mono 28 */
    const lv_font_t *disp_s;    /* Orbitron 18: labels, chips */
    const lv_font_t *disp_m;    /* Orbitron 24: titles */
    const lv_font_t *disp_l;    /* Orbitron 36: headings */
    const lv_font_t *disp_xl;   /* Orbitron 56: hero numbers */
    const lv_font_t *icon_s;    /* MDI 24: status bar */
    const lv_font_t *icon_m;    /* MDI 40 */
    const lv_font_t *icon_l;    /* MDI 96: launcher tiles */
} deck_fonts_t;

extern deck_fonts_t g_font;

#define DECK_ACCENT_COUNT 4

/* Load fonts and the saved accent. Call once after LVGL is up. */
void deck_theme_init(void);

void        deck_theme_set_accent(int index);
int         deck_theme_accent(void);
const char *deck_theme_accent_name(int index);
lv_color_t  deck_theme_accent_color(int index);

#ifdef __cplusplus
}
#endif
