#include "deck_widgets.h"

#include <stdlib.h>
#include <string.h>

#include "deck_theme.h"

#define GLOW_GAP 4      /* focus glow sits this far outside the outline */
#define OUTLINE_W 2

typedef struct {
    uint8_t cuts;
    int32_t cut;
    lv_color_t outline;
    lv_color_t fill;
    bool tab;
    bool hazard;
} panel_data_t;

/* ---- Low-level drawing helpers ------------------------------------------- */

static void fill_rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c,
                      lv_opa_t opa)
{
    if (x2 < x1 || y2 < y1) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color      = c;
    d.bg_opa        = opa;
    d.border_width  = 0;
    d.outline_width = 0;
    d.shadow_width  = 0;
    d.radius        = 0;
    lv_area_t a     = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

static void fill_tri(lv_layer_t *layer, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t cx, int32_t cy,
                     lv_color_t c)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0].x = ax;
    d.p[0].y = ay;
    d.p[1].x = bx;
    d.p[1].y = by;
    d.p[2].x = cx;
    d.p[2].y = cy;
    d.color  = c;
    d.opa    = LV_OPA_COVER;
    lv_draw_triangle(layer, &d);
}

static void line_ex(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w, lv_color_t c,
                    lv_opa_t opa, bool round)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x        = x1;
    d.p1.y        = y1;
    d.p2.x        = x2;
    d.p2.y        = y2;
    d.width       = w;
    d.color       = c;
    d.opa         = opa;
    d.round_start = round;
    d.round_end   = round;
    lv_draw_line(layer, &d);
}

/* Round caps close the joints where outline segments meet. */
static void line(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t w, lv_color_t c,
                 lv_opa_t opa)
{
    line_ex(layer, x1, y1, x2, y2, w, c, opa, true);
}

/* Vertices of the chamfered outline of area `a`, clockwise from top-left. */
static int outline_points(const lv_area_t *a, uint8_t cuts, int32_t c, lv_point_t *pts)
{
    int n = 0;
    if (cuts & DECK_CUT_TL) {
        pts[n++] = (lv_point_t){a->x1, a->y1 + c};
        pts[n++] = (lv_point_t){a->x1 + c, a->y1};
    } else {
        pts[n++] = (lv_point_t){a->x1, a->y1};
    }
    if (cuts & DECK_CUT_TR) {
        pts[n++] = (lv_point_t){a->x2 - c, a->y1};
        pts[n++] = (lv_point_t){a->x2, a->y1 + c};
    } else {
        pts[n++] = (lv_point_t){a->x2, a->y1};
    }
    if (cuts & DECK_CUT_BR) {
        pts[n++] = (lv_point_t){a->x2, a->y2 - c};
        pts[n++] = (lv_point_t){a->x2 - c, a->y2};
    } else {
        pts[n++] = (lv_point_t){a->x2, a->y2};
    }
    if (cuts & DECK_CUT_BL) {
        pts[n++] = (lv_point_t){a->x1 + c, a->y2};
        pts[n++] = (lv_point_t){a->x1, a->y2 - c};
    } else {
        pts[n++] = (lv_point_t){a->x1, a->y2};
    }
    return n;
}

static void stroke_outline(lv_layer_t *layer, const lv_area_t *a, uint8_t cuts, int32_t c, int32_t w,
                           lv_color_t col, lv_opa_t opa)
{
    lv_point_t p[8];
    int n = outline_points(a, cuts, c, p);
    for (int i = 0; i < n; i++) {
        const lv_point_t *p1 = &p[i];
        const lv_point_t *p2 = &p[(i + 1) % n];
        line(layer, p1->x, p1->y, p2->x, p2->y, w, col, opa);
    }
}

/* Opaque fill of a chamfered shape without anti-aliasing seams: each cut
 * corner is an oversized triangle whose only exposed edge is the diagonal;
 * the axis-aligned bands drawn afterwards cover its inner edges. */
static void fill_chamfer(lv_layer_t *layer, const lv_area_t *a, uint8_t cuts, int32_t c, lv_color_t col)
{
    int32_t x1 = a->x1, y1 = a->y1, x2 = a->x2, y2 = a->y2;
    if (cuts & DECK_CUT_TL) fill_tri(layer, x1, y1 + c, x1 + c, y1, x1 + 2 * c, y1 + 2 * c, col);
    if (cuts & DECK_CUT_TR) fill_tri(layer, x2 - c, y1, x2 + 1, y1 + c, x2 - 2 * c, y1 + 2 * c, col);
    if (cuts & DECK_CUT_BR) fill_tri(layer, x2 + 1, y2 - c, x2 - c, y2 + 1, x2 - 2 * c, y2 - 2 * c, col);
    if (cuts & DECK_CUT_BL) fill_tri(layer, x1 + c, y2 + 1, x1, y2 - c, x1 + 2 * c, y2 - 2 * c, col);

    int32_t tl = (cuts & DECK_CUT_TL) ? c : 0;
    int32_t tr = (cuts & DECK_CUT_TR) ? c : 0;
    int32_t br = (cuts & DECK_CUT_BR) ? c : 0;
    int32_t bl = (cuts & DECK_CUT_BL) ? c : 0;
    int32_t top_h = LV_MAX(tl, tr);
    int32_t bot_h = LV_MAX(br, bl);

    fill_rect(layer, x1, y1 + top_h, x2, y2 - bot_h, col, LV_OPA_COVER);
    if (top_h) fill_rect(layer, x1 + tl, y1, x2 - tr, y1 + top_h - 1, col, LV_OPA_COVER);
    if (bot_h) fill_rect(layer, x1 + bl, y2 - bot_h + 1, x2 - br, y2, col, LV_OPA_COVER);
}

/* ---- Panel --------------------------------------------------------------- */

static void panel_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *obj        = lv_event_get_target_obj(e);
    panel_data_t *pd     = (panel_data_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_DELETE) {
        free(pd);
        return;
    }
    if (code == LV_EVENT_STATE_CHANGED) {
        /* No style depends on these states, so LVGL would not redraw. */
        lv_obj_invalidate(obj);
        return;
    }
    if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        lv_event_set_ext_draw_size(e, GLOW_GAP + OUTLINE_W + 1);
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    bool pressed = lv_obj_has_state(obj, LV_STATE_PRESSED);
    bool focused = lv_obj_has_state(obj, LV_STATE_FOCUS_KEY);
    bool checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
    bool lit     = pressed || focused || checked;
    int32_t c    = pd->cut;

    fill_chamfer(layer, &a, pd->cuts, c, pressed ? g_pal.panel_hi : pd->fill);

    if (pd->hazard) {
        /* Yellow stripes along the bottom, kept clear of the cut corners. */
        int32_t y = a.y2 - 5;
        for (int32_t x = a.x1 + c + 12; x + 14 < a.x2 - c - 4; x += 16) {
            line_ex(layer, x, y, x + 8, y - 8, 6, g_pal.warn, LV_OPA_50, false);
        }
    }

    lv_area_t o = a;
    lv_area_increase(&o, -OUTLINE_W / 2, -OUTLINE_W / 2);
    stroke_outline(layer, &o, pd->cuts, c, OUTLINE_W, lit ? g_pal.accent : pd->outline, LV_OPA_COVER);

    if (focused || pressed) {
        lv_area_t g = a;
        lv_area_increase(&g, GLOW_GAP, GLOW_GAP);
        stroke_outline(layer, &g, pd->cuts, c + GLOW_GAP / 2, 2, g_pal.accent, LV_OPA_40);
    }

    if (pd->tab) {
        int32_t tx = a.x1 + ((pd->cuts & DECK_CUT_TL) ? c : 0) + 10;
        fill_rect(layer, tx, a.y1, tx + 56, a.y1 + 3, lit ? g_pal.accent : g_pal.accent2, LV_OPA_COVER);
    }
}

lv_obj_t *deck_panel(lv_obj_t *parent, uint8_t cuts, int32_t cut_px)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    panel_data_t *pd = (panel_data_t *)calloc(1, sizeof(panel_data_t));
    pd->cuts         = cuts;
    pd->cut          = cut_px;
    pd->outline      = g_pal.line;
    pd->fill         = g_pal.panel;
    lv_obj_add_event_cb(p, panel_event, LV_EVENT_ALL, pd);
    lv_obj_refresh_ext_draw_size(p);
    return p;
}

static panel_data_t *panel_data(lv_obj_t *panel)
{
    /* The panel_event callback is always the first one registered. */
    lv_event_dsc_t *dsc = lv_obj_get_event_dsc(panel, 0);
    return dsc ? (panel_data_t *)lv_event_dsc_get_user_data(dsc) : NULL;
}

void deck_panel_set_outline(lv_obj_t *panel, lv_color_t color)
{
    panel_data_t *pd = panel_data(panel);
    if (pd) {
        pd->outline = color;
        lv_obj_invalidate(panel);
    }
}

void deck_panel_set_fill(lv_obj_t *panel, lv_color_t color)
{
    panel_data_t *pd = panel_data(panel);
    if (pd) {
        pd->fill = color;
        lv_obj_invalidate(panel);
    }
}

void deck_panel_set_tab(lv_obj_t *panel, bool on)
{
    panel_data_t *pd = panel_data(panel);
    if (pd) {
        pd->tab = on;
        lv_obj_invalidate(panel);
    }
}

void deck_panel_set_hazard(lv_obj_t *panel, bool on)
{
    panel_data_t *pd = panel_data(panel);
    if (pd) {
        pd->hazard = on;
        lv_obj_invalidate(panel);
    }
}

/* ---- Button -------------------------------------------------------------- */

static void button_label_color(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    bool lit = lv_obj_has_state(btn, LV_STATE_PRESSED) || lv_obj_has_state(btn, LV_STATE_FOCUS_KEY) ||
               lv_obj_has_state(btn, LV_STATE_CHECKED);
    /* Text color is inherited, so setting it on the button recolors any
     * label or icon row inside. */
    lv_obj_set_style_text_color(btn, lit ? g_pal.accent : g_pal.text, 0);
}

lv_obj_t *deck_button(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *b = deck_panel(parent, DECK_CUT_DIAG, 10);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(b, 52, 0); /* comfortable touch target */
    lv_obj_set_style_min_width(b, 96, 0);

    lv_obj_set_style_text_color(b, g_pal.text, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, font ? font : g_font.disp_s, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);

    lv_obj_add_event_cb(b, button_label_color, LV_EVENT_STATE_CHANGED, NULL);
    lv_group_t *g = lv_group_get_default();
    if (g) lv_group_add_obj(g, b);
    return b;
}

/* ---- Small helpers ------------------------------------------------------- */

static const lv_font_t *icon_font_for(const lv_font_t *font)
{
    return lv_font_get_line_height(font) >= 34 ? g_font.icon_m : g_font.icon_s;
}

lv_obj_t *deck_icon_text(lv_obj_t *parent, const char *icon, const char *text, const lv_font_t *font,
                         lv_color_t color)
{
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_style_text_color(row, color, 0);

    lv_obj_t *i = lv_label_create(row);
    lv_obj_set_style_text_font(i, icon_font_for(font), 0);
    lv_label_set_text(i, icon ? icon : "");
    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, font, 0);
    lv_label_set_text(t, text ? text : "");
    return row;
}

void deck_icon_text_set(lv_obj_t *row, const char *icon, const char *text, lv_color_t color)
{
    lv_obj_t *i = lv_obj_get_child(row, 0);
    lv_obj_t *t = lv_obj_get_child(row, 1);
    if (icon && strcmp(lv_label_get_text(i), icon) != 0) lv_label_set_text(i, icon);
    if (text && strcmp(lv_label_get_text(t), text) != 0) lv_label_set_text(t, text);
    lv_obj_set_style_text_color(row, color, 0);
}

lv_obj_t *deck_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

lv_obj_t *deck_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t *deck_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *l = deck_label(row, g_font.disp_s, g_pal.accent, "");
    lv_label_set_text_fmt(l, "// %s", title);

    lv_obj_t *rule = lv_obj_create(row);
    lv_obj_remove_style_all(rule);
    lv_obj_set_height(rule, 2);
    lv_obj_set_flex_grow(rule, 1);
    lv_obj_set_style_bg_color(rule, g_pal.line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    return row;
}

lv_obj_t *deck_chip(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *c = deck_panel(parent, DECK_CUT_BR, 6);
    deck_panel_set_outline(c, color);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(c, 10, 0);
    lv_obj_set_style_pad_ver(c, 4, 0);
    deck_label(c, g_font.mono_s, color, text);
    return c;
}

/* ---- HUD grid ------------------------------------------------------------ */

#define GRID_STEP 40
#define CROSS_STEP 160

static void grid_draw(lv_event_t *e)
{
    lv_obj_t *obj     = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    /* Only emit primitives that intersect the area being redrawn. */
    const lv_area_t *lc = &layer->_clip_area;
    lv_area_t clip = {LV_MAX(a.x1, lc->x1), LV_MAX(a.y1, lc->y1), LV_MIN(a.x2, lc->x2), LV_MIN(a.y2, lc->y2)};
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) return;

    fill_rect(layer, clip.x1, clip.y1, clip.x2, clip.y2, g_pal.bg, LV_OPA_COVER);

    int32_t x0 = a.x1 + ((clip.x1 - a.x1 + GRID_STEP - 1) / GRID_STEP) * GRID_STEP;
    for (int32_t x = x0; x <= clip.x2; x += GRID_STEP) {
        fill_rect(layer, x, clip.y1, x, clip.y2, g_pal.grid, LV_OPA_COVER);
    }
    int32_t y0 = a.y1 + ((clip.y1 - a.y1 + GRID_STEP - 1) / GRID_STEP) * GRID_STEP;
    for (int32_t y = y0; y <= clip.y2; y += GRID_STEP) {
        fill_rect(layer, clip.x1, y, clip.x2, y, g_pal.grid, LV_OPA_COVER);
    }

    /* Small accent crosshairs on a coarser lattice. */
    for (int32_t x = a.x1 + CROSS_STEP; x < a.x2; x += CROSS_STEP) {
        if (x + 6 < clip.x1 || x - 6 > clip.x2) continue;
        for (int32_t y = a.y1 + CROSS_STEP; y < a.y2; y += CROSS_STEP) {
            if (y + 6 < clip.y1 || y - 6 > clip.y2) continue;
            fill_rect(layer, x - 6, y, x + 6, y, g_pal.accent, LV_OPA_30);
            fill_rect(layer, x, y - 6, x, y + 6, g_pal.accent, LV_OPA_30);
        }
    }
}

void deck_grid_bg(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, grid_draw, LV_EVENT_DRAW_MAIN, NULL);
}
