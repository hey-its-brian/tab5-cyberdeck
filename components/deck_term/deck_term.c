#include "deck_term.h"

#include <stdlib.h>
#include <string.h>

#include "deck_assets.h"
#include "deck_hal.h"
#include "deck_theme.h"
#include "term_glyphs.h"
#include "vterm.h"

#define FONT_PX 16
#define SB_DEFAULT 2000
#define BLINK_MS 530
#define PAD 6

/* Compact scrollback cell: codepoint, colors, attributes. */
typedef struct {
    uint32_t ch;
    uint8_t fg[3], bg[3];
    uint8_t attr; /* bit0 bold, bit1 underline, bit2 reverse, bit3 default bg */
} sb_cell_t;

struct deck_term {
    lv_obj_t *obj;
    VTerm *vt;
    VTermScreen *vs;
    deck_term_cfg_t cfg;
    const lv_font_t *font;
    int cw, ch;          /* cell size in px */
    int rows, cols;
    int x0, y0;          /* grid origin inside the object */

    sb_cell_t *sb;       /* ring of sb_cap lines, sb_w cells each */
    int sb_cap, sb_w, sb_count, sb_head; /* head = next write slot */
    int view;            /* lines scrolled back, 0 = live */

    VTermPos cursor;
    bool cursor_visible;
    bool cursor_blink;
    bool blink_on;
    lv_timer_t *blink;
    bool sticky_ctrl;
};

static const lv_font_t *s_font;

/* Cyberpunk-leaning ANSI palette: readable on near-black. */
static const uint32_t s_palette[16] = {
    0x101320, 0xFF3B5C, 0x39FF14, 0xF3E600, 0x3D8BFF, 0xFF2A6D, 0x00F0FF, 0xC8D0E0,
    0x5F6884, 0xFF6B85, 0x7DFF5C, 0xFFF35C, 0x7AB0FF, 0xFF6FA0, 0x6BF7FF, 0xFFFFFF,
};
#define TERM_FG 0xD8E1F0
#define TERM_BG 0x04050A

/* ---- Glyph availability -------------------------------------------------- */

static bool have_glyph(uint32_t cp)
{
    size_t lo = 0, hi = TERM_GLYPH_RANGE_COUNT;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < g_term_glyph_ranges[mid][0]) {
            hi = mid;
        } else if (cp > g_term_glyph_ranges[mid][1]) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

static int utf8_put(char *o, uint32_t cp)
{
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ---- Cell access (live screen or scrollback) ----------------------------- */

typedef struct {
    uint32_t ch;
    uint32_t fg, bg; /* 0xRRGGBB */
    bool bold, underline, default_bg;
    int width;
} cell_t;

static uint32_t rgb_of(VTermScreen *vs, VTermColor c, bool is_fg)
{
    if (is_fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) return TERM_FG;
    if (!is_fg && VTERM_COLOR_IS_DEFAULT_BG(&c)) return TERM_BG;
    vterm_screen_convert_color_to_rgb(vs, &c);
    return ((uint32_t)c.rgb.red << 16) | ((uint32_t)c.rgb.green << 8) | c.rgb.blue;
}

static void screen_cell(deck_term_t *t, int row, int col, cell_t *o)
{
    VTermScreenCell c;
    VTermPos p = {row, col};
    if (!vterm_screen_get_cell(t->vs, p, &c)) {
        memset(o, 0, sizeof(*o));
        o->fg = TERM_FG;
        o->bg = TERM_BG;
        o->default_bg = true;
        o->width = 1;
        return;
    }
    o->ch         = c.chars[0];
    o->width      = c.width > 0 ? c.width : 1;
    o->bold       = c.attrs.bold;
    o->underline  = c.attrs.underline != 0;
    o->fg         = rgb_of(t->vs, c.fg, true);
    o->bg         = rgb_of(t->vs, c.bg, false);
    o->default_bg = VTERM_COLOR_IS_DEFAULT_BG(&c.bg);
    if (c.attrs.reverse) {
        uint32_t x = o->fg;
        o->fg      = o->bg;
        o->bg      = x;
        o->default_bg = false;
    }
}

static void sb_cell(deck_term_t *t, int line_from_newest, int col, cell_t *o)
{
    memset(o, 0, sizeof(*o));
    o->width = 1;
    o->fg    = TERM_FG;
    o->bg    = TERM_BG;
    o->default_bg = true;
    if (line_from_newest < 0 || line_from_newest >= t->sb_count || col >= t->sb_w) return;
    int slot           = (t->sb_head - 1 - line_from_newest + t->sb_cap) % t->sb_cap;
    const sb_cell_t *s = &t->sb[(size_t)slot * t->sb_w + col];
    o->ch              = s->ch;
    o->fg              = ((uint32_t)s->fg[0] << 16) | (s->fg[1] << 8) | s->fg[2];
    o->bg              = ((uint32_t)s->bg[0] << 16) | (s->bg[1] << 8) | s->bg[2];
    o->bold            = s->attr & 1;
    o->underline       = s->attr & 2;
    o->default_bg      = s->attr & 8;
}

/* Visible row r: scrollback lines first when scrolled back. */
static void view_cell(deck_term_t *t, int r, int col, cell_t *o)
{
    if (r < t->view) {
        sb_cell(t, t->view - 1 - r, col, o);
    } else {
        screen_cell(t, r - t->view, col, o);
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t rgb, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb);
    d.bg_opa   = opa;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

static void draw_run(deck_term_t *t, lv_layer_t *layer, int row, int c0, int c1, const cell_t *style, const char *txt)
{
    lv_area_t o;
    lv_obj_get_coords(t->obj, &o);
    int32_t x = o.x1 + t->x0 + c0 * t->cw;
    int32_t y = o.y1 + t->y0 + row * t->ch;
    if (!style->default_bg) fill(layer, x, y, x + (c1 - c0) * t->cw - 1, y + t->ch - 1, style->bg, LV_OPA_COVER);
    if (txt[0]) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font  = t->font;
        d.color = lv_color_hex(style->fg);
        d.text  = txt;
        d.text_local = 1;
        if (style->underline) d.decor = LV_TEXT_DECOR_UNDERLINE;
        lv_area_t a = {x, y, x + (c1 - c0) * t->cw + t->cw, y + t->ch - 1};
        lv_draw_label(layer, &d, &a);
    }
}

static void draw_event(lv_event_t *e)
{
    deck_term_t *t    = (deck_term_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t o;
    lv_obj_get_coords(t->obj, &o);
    const lv_area_t *clip = &layer->_clip_area;

    fill(layer, LV_MAX(o.x1, clip->x1), LV_MAX(o.y1, clip->y1), LV_MIN(o.x2, clip->x2), LV_MIN(o.y2, clip->y2), TERM_BG,
         LV_OPA_COVER);

    int r0 = (clip->y1 - o.y1 - t->y0) / t->ch;
    int r1 = (clip->y2 - o.y1 - t->y0) / t->ch;
    if (r0 < 0) r0 = 0;
    if (r1 >= t->rows) r1 = t->rows - 1;

    static char txt[1024];
    for (int r = r0; r <= r1; r++) {
        cell_t run, c;
        int start = 0;
        size_t n  = 0;
        view_cell(t, r, 0, &run);
        for (int col = 0; col <= t->cols; col++) {
            bool end = col == t->cols;
            if (!end) view_cell(t, r, col, &c);
            bool same = !end && c.fg == run.fg && c.bg == run.bg && c.underline == run.underline &&
                        c.default_bg == run.default_bg;
            if (!same) {
                txt[n] = '\0';
                draw_run(t, layer, r, start, col, &run, txt);
                if (end) break;
                run   = c;
                start = col;
                n     = 0;
            }
            uint32_t cp = c.ch ? c.ch : ' ';
            if (!have_glyph(cp)) cp = '?';
            if (n + 4 < sizeof(txt)) n += (size_t)utf8_put(txt + n, cp);
            if (c.width == 2 && col + 1 < t->cols) {
                col++; /* the font has no wide glyphs; pad the second cell */
                if (n + 1 < sizeof(txt)) txt[n++] = ' ';
            }
        }
    }

    /* Cursor: a translucent accent block over the cell. */
    if (t->view == 0 && t->cursor_visible && t->blink_on && t->cursor.row >= r0 && t->cursor.row <= r1) {
        int32_t x = o.x1 + t->x0 + t->cursor.col * t->cw;
        int32_t y = o.y1 + t->y0 + t->cursor.row * t->ch;
        lv_color_t a = g_pal.accent;
        fill(layer, x, y, x + t->cw - 1, y + t->ch - 1, lv_color_to_u32(a) & 0xFFFFFF, LV_OPA_60);
    }

    /* Scrolled back: say so along the bottom edge. */
    if (t->view > 0) {
        fill(layer, o.x1, o.y2 - 3, o.x2, o.y2, lv_color_to_u32(g_pal.accent2) & 0xFFFFFF, LV_OPA_COVER);
    }
}

static void invalidate_cells(deck_term_t *t, int r0, int c0, int r1, int c1)
{
    lv_area_t o;
    lv_obj_get_coords(t->obj, &o);
    lv_area_t a = {o.x1 + t->x0 + c0 * t->cw, o.y1 + t->y0 + r0 * t->ch, o.x1 + t->x0 + c1 * t->cw + t->cw,
                   o.y1 + t->y0 + r1 * t->ch - 1};
    lv_obj_invalidate_area(t->obj, &a);
}

/* ---- libvterm callbacks -------------------------------------------------- */

static int cb_damage(VTermRect r, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    if (t->view == 0) invalidate_cells(t, r.start_row, r.start_col, r.end_row, r.end_col);
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    if (t->view == 0) {
        invalidate_cells(t, LV_MIN(dest.start_row, src.start_row), LV_MIN(dest.start_col, src.start_col),
                         LV_MAX(dest.end_row, src.end_row), LV_MAX(dest.end_col, src.end_col));
    }
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    (void)visible;
    invalidate_cells(t, old.row, old.col, old.row + 1, old.col + 1);
    t->cursor  = pos;
    t->blink_on = true; /* show it right away after it moves */
    invalidate_cells(t, pos.row, pos.col, pos.row + 1, pos.col + 1);
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    switch (prop) {
        case VTERM_PROP_CURSORVISIBLE:
            t->cursor_visible = val->boolean;
            invalidate_cells(t, t->cursor.row, t->cursor.col, t->cursor.row + 1, t->cursor.col + 1);
            break;
        case VTERM_PROP_CURSORBLINK: t->cursor_blink = val->boolean; break;
        case VTERM_PROP_TITLE:
            if (t->cfg.title && val->string.initial && val->string.final) {
                char buf[96];
                size_t n = val->string.len < sizeof(buf) - 1 ? val->string.len : sizeof(buf) - 1;
                memcpy(buf, val->string.str, n);
                buf[n] = '\0';
                t->cfg.title(buf, t->cfg.user);
            }
            break;
        default: break;
    }
    return 1;
}

static int cb_bell(void *user)
{
    (void)user;
    return 1;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    if (t->sb == NULL) return 0;
    sb_cell_t *line = &t->sb[(size_t)t->sb_head * t->sb_w];
    for (int i = 0; i < t->sb_w; i++) {
        sb_cell_t *s = &line[i];
        memset(s, 0, sizeof(*s));
        if (i >= cols) {
            s->attr = 8;
            s->fg[0] = (TERM_FG >> 16) & 0xFF;
            s->fg[1] = (TERM_FG >> 8) & 0xFF;
            s->fg[2] = TERM_FG & 0xFF;
            continue;
        }
        VTermScreenCell c = cells[i];
        uint32_t fg       = rgb_of(t->vs, c.fg, true);
        uint32_t bg       = rgb_of(t->vs, c.bg, false);
        bool dbg          = VTERM_COLOR_IS_DEFAULT_BG(&c.bg);
        if (c.attrs.reverse) {
            uint32_t x = fg;
            fg         = bg;
            bg         = x;
            dbg        = false;
        }
        s->ch    = c.chars[0];
        s->fg[0] = (fg >> 16) & 0xFF;
        s->fg[1] = (fg >> 8) & 0xFF;
        s->fg[2] = fg & 0xFF;
        s->bg[0] = (bg >> 16) & 0xFF;
        s->bg[1] = (bg >> 8) & 0xFF;
        s->bg[2] = bg & 0xFF;
        s->attr  = (c.attrs.bold ? 1 : 0) | (c.attrs.underline ? 2 : 0) | (dbg ? 8 : 0);
    }
    t->sb_head = (t->sb_head + 1) % t->sb_cap;
    if (t->sb_count < t->sb_cap) t->sb_count++;
    if (t->view > 0 && t->view < t->sb_count) t->view++; /* keep the scrolled view still */
    return 1;
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    if (t->sb_count == 0) return 0;
    t->sb_head      = (t->sb_head - 1 + t->sb_cap) % t->sb_cap;
    t->sb_count--;
    const sb_cell_t *line = &t->sb[(size_t)t->sb_head * t->sb_w];
    for (int i = 0; i < cols; i++) {
        memset(&cells[i], 0, sizeof(cells[i]));
        cells[i].width = 1;
        if (i < t->sb_w) {
            cells[i].chars[0] = line[i].ch;
            vterm_color_rgb(&cells[i].fg, line[i].fg[0], line[i].fg[1], line[i].fg[2]);
            vterm_color_rgb(&cells[i].bg, line[i].bg[0], line[i].bg[1], line[i].bg[2]);
        }
    }
    return 1;
}

static int cb_sb_clear(void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    t->sb_count = t->sb_head = t->view = 0;
    return 1;
}

static const VTermScreenCallbacks s_cbs = {
    .damage      = cb_damage,
    .moverect    = cb_moverect,
    .movecursor  = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell        = cb_bell,
    .sb_pushline = cb_sb_pushline,
    .sb_popline  = cb_sb_popline,
    .sb_clear    = cb_sb_clear,
};

static void output_cb(const char *s, size_t len, void *user)
{
    deck_term_t *t = (deck_term_t *)user;
    if (t->cfg.output) t->cfg.output(s, len, t->cfg.user);
}

/* ---- Geometry ------------------------------------------------------------ */

static void relayout(deck_term_t *t)
{
    int32_t w = lv_obj_get_width(t->obj), h = lv_obj_get_height(t->obj);
    int cols = (int)((w - 2 * PAD) / t->cw), rows = (int)((h - 2 * PAD) / t->ch);
    if (cols < 20) cols = 20;
    if (rows < 5) rows = 5;
    t->x0 = (int)((w - cols * t->cw) / 2);
    t->y0 = (int)((h - rows * t->ch) / 2);
    if (cols == t->cols && rows == t->rows) return;
    t->cols = cols;
    t->rows = rows;
    vterm_set_size(t->vt, rows, cols);
    vterm_screen_flush_damage(t->vs);
    lv_obj_invalidate(t->obj);
    if (t->cfg.resized) t->cfg.resized(cols, rows, t->cfg.user);
}

static void size_event(lv_event_t *e) { relayout((deck_term_t *)lv_event_get_user_data(e)); }

static void blink_tick(lv_timer_t *tm)
{
    deck_term_t *t = (deck_term_t *)lv_timer_get_user_data(tm);
    t->blink_on    = t->cursor_blink ? !t->blink_on : true;
    invalidate_cells(t, t->cursor.row, t->cursor.col, t->cursor.row + 1, t->cursor.col + 1);
}

/* Touch: drag vertically to scroll back through history. */
static void press_event(lv_event_t *e)
{
    deck_term_t *t   = (deck_term_t *)lv_event_get_user_data(e);
    static int32_t acc;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        acc = 0;
        return;
    }
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    acc += v.y;
    int lines = acc / t->ch;
    if (lines) {
        acc -= lines * t->ch;
        deck_term_scroll(t, lines);
    }
}

/* ---- Public -------------------------------------------------------------- */

deck_term_t *deck_term_create(lv_obj_t *parent, const deck_term_cfg_t *cfg)
{
    if (s_font == NULL) {
        size_t sz = 0;
        const uint8_t *data = deck_asset(DECK_ASSET_FONT_TERM, &sz);
        s_font = data ? lv_tiny_ttf_create_data_ex(data, sz, FONT_PX, LV_FONT_KERNING_NONE, 256) : NULL;
        if (s_font == NULL) s_font = g_font.mono_s;
    }

    deck_term_t *t = (deck_term_t *)calloc(1, sizeof(deck_term_t));
    t->cfg         = *cfg;
    t->font        = s_font;
    lv_font_glyph_dsc_t g;
    t->cw = lv_font_get_glyph_dsc(t->font, &g, 'M', 0) ? g.adv_w : FONT_PX / 2;
    t->ch = lv_font_get_line_height(t->font) + 2;
    t->cursor_visible = true;
    t->blink_on       = true;

    t->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(t->obj);
    lv_obj_set_size(t->obj, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(t->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->obj, draw_event, LV_EVENT_DRAW_MAIN, t);
    lv_obj_add_event_cb(t->obj, size_event, LV_EVENT_SIZE_CHANGED, t);
    lv_obj_add_event_cb(t->obj, press_event, LV_EVENT_PRESSED, t);
    lv_obj_add_event_cb(t->obj, press_event, LV_EVENT_PRESSING, t);

    t->vt = vterm_new(24, 80);
    vterm_set_utf8(t->vt, 1);
    vterm_output_set_callback(t->vt, output_cb, t);
    t->vs = vterm_obtain_screen(t->vt);
    vterm_screen_set_callbacks(t->vs, &s_cbs, t);
    vterm_screen_set_damage_merge(t->vs, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(t->vs, 1);

    VTermState *st = vterm_obtain_state(t->vt);
    VTermColor fg, bg;
    vterm_color_rgb(&fg, (TERM_FG >> 16) & 0xFF, (TERM_FG >> 8) & 0xFF, TERM_FG & 0xFF);
    vterm_color_rgb(&bg, (TERM_BG >> 16) & 0xFF, (TERM_BG >> 8) & 0xFF, TERM_BG & 0xFF);
    vterm_state_set_default_colors(st, &fg, &bg);
    for (int i = 0; i < 16; i++) {
        VTermColor c;
        vterm_color_rgb(&c, (s_palette[i] >> 16) & 0xFF, (s_palette[i] >> 8) & 0xFF, s_palette[i] & 0xFF);
        vterm_state_set_palette_color(st, i, &c);
    }
    vterm_screen_reset(t->vs, 1);

    /* Scrollback lives in PSRAM on the device (malloc prefers it for large
     * blocks); sized for the widest grid the screen can hold. */
    t->sb_cap = cfg->scrollback_lines > 0 ? cfg->scrollback_lines : SB_DEFAULT;
    t->sb_w   = (HAL_SCREEN_W / t->cw) + 1;
    t->sb     = (sb_cell_t *)calloc((size_t)t->sb_cap * t->sb_w, sizeof(sb_cell_t));

    lv_obj_update_layout(t->obj);
    relayout(t);
    t->blink = lv_timer_create(blink_tick, BLINK_MS, t);
    return t;
}

void deck_term_destroy(deck_term_t *t)
{
    if (t == NULL) return;
    if (t->blink) lv_timer_delete(t->blink);
    vterm_free(t->vt);
    free(t->sb);
    free(t);
}

lv_obj_t *deck_term_obj(deck_term_t *t) { return t->obj; }

void deck_term_feed(deck_term_t *t, const char *data, size_t len)
{
    vterm_input_write(t->vt, data, len);
    vterm_screen_flush_damage(t->vs);
}

void deck_term_size(deck_term_t *t, int *cols, int *rows)
{
    *cols = t->cols;
    *rows = t->rows;
}

void deck_term_scroll(deck_term_t *t, int lines)
{
    int v = t->view + lines;
    if (v < 0) v = 0;
    if (v > t->sb_count) v = t->sb_count;
    if (v != t->view) {
        t->view = v;
        lv_obj_invalidate(t->obj);
    }
}

void deck_term_scroll_live(deck_term_t *t) { deck_term_scroll(t, -t->view); }

void deck_term_set_sticky_ctrl(deck_term_t *t, bool on) { t->sticky_ctrl = on; }
bool deck_term_sticky_ctrl(deck_term_t *t) { return t->sticky_ctrl; }

void deck_term_send_vkey(deck_term_t *t, int key, int mods)
{
    deck_term_scroll_live(t);
    vterm_keyboard_key(t->vt, (VTermKey)key, (VTermModifier)mods);
}

void deck_term_send_text(deck_term_t *t, const char *s)
{
    deck_term_scroll_live(t);
    while (*s) vterm_keyboard_unichar(t->vt, (unsigned char)*s++, VTERM_MOD_NONE);
}

bool deck_term_key(deck_term_t *t, const deck_key_t *k)
{
    bool shift = k->mods & DECK_MOD_SHIFT, ctrl = (k->mods & DECK_MOD_CTRL) || t->sticky_ctrl,
         alt   = k->mods & DECK_MOD_ALT;

    /* Shift+Up/Down page through scrollback locally. */
    if (shift && !ctrl && !alt && (k->code == HID_UP || k->code == HID_DOWN)) {
        deck_term_scroll(t, k->code == HID_UP ? t->rows / 2 : -t->rows / 2);
        return true;
    }

    VTermModifier mod = (shift ? VTERM_MOD_SHIFT : 0) | (ctrl ? VTERM_MOD_CTRL : 0) | (alt ? VTERM_MOD_ALT : 0);
    VTermKey vk       = VTERM_KEY_NONE;
    switch (k->code) {
        case HID_ENTER: vk = VTERM_KEY_ENTER; break;
        case HID_ESC: vk = VTERM_KEY_ESCAPE; break;
        case HID_BACKSPACE: vk = VTERM_KEY_BACKSPACE; break;
        case HID_TAB: vk = VTERM_KEY_TAB; break;
        case HID_DELETE: vk = VTERM_KEY_DEL; break;
        case HID_UP: vk = VTERM_KEY_UP; break;
        case HID_DOWN: vk = VTERM_KEY_DOWN; break;
        case HID_LEFT: vk = VTERM_KEY_LEFT; break;
        case HID_RIGHT: vk = VTERM_KEY_RIGHT; break;
        case 0x49: vk = VTERM_KEY_INS; break;
        case 0x4A: vk = VTERM_KEY_HOME; break;
        case 0x4B: vk = VTERM_KEY_PAGEUP; break;
        case 0x4D: vk = VTERM_KEY_END; break;
        case 0x4E: vk = VTERM_KEY_PAGEDOWN; break;
        default:
            if (k->code >= 0x3A && k->code <= 0x45) vk = VTERM_KEY_FUNCTION(k->code - 0x3A + 1);
            break;
    }

    deck_term_scroll_live(t);
    if (vk != VTERM_KEY_NONE) {
        vterm_keyboard_key(t->vt, vk, mod);
    } else {
        /* Character keys: the shifted character carries Shift itself. */
        uint32_t c = deck_input_translate(k->code, k->mods & DECK_MOD_SHIFT);
        if (c < 0x20) return false; /* nothing printable on this key */
        VTermModifier cm = (ctrl ? VTERM_MOD_CTRL : 0) | (alt ? VTERM_MOD_ALT : 0);
        if (ctrl && c >= 'A' && c <= 'Z') c += 'a' - 'A';
        vterm_keyboard_unichar(t->vt, c, cm);
    }
    if (t->sticky_ctrl) t->sticky_ctrl = false;
    return true;
}
