/*
 * SENTINEL's frame: the four things round every screen (where you are and
 * why, the body, the action bar, the alert line), the stack of screens, the
 * menu pages and the few widgets screens share.
 *
 * Two looks, and only two. Something that can be tapped (a button, a tile,
 * a list row, the alert line while it has something to open) is a filled
 * shape with a bright border and rounded corners. Everything else is plain
 * text on the background. A screen never draws a box that is not a control.
 */
#include "sn_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "deck_attr.h"
#include "deck_modal.h"
#include "deck_notice.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define STACK_MAX 5
#define ACT_MAX   7
#define TILE_MAX  8
#define TICK_MS   500
#define BAR_H     64
#define STRIP_H   52 /* the height of the info bar on the DECK//OS home screen */
#define ROW_H     50
#define WARN_HOLD_MS  120000u /* how long a warning stays on the alert line */
#define ALERT_HOLD_MS 300000u

typedef struct {
    uint8_t   hid;
    sn_fn_t   fn;
    lv_obj_t *btn, *label;
    bool      enabled, danger;
} action_t;

static struct {
    lv_obj_t          *parent;
    const sn_screen_t *stack[STACK_MAX];
    int                sel[STACK_MAX];
    int                depth;
    bool               built;   /* the leaf on top was built, so it must be destroyed */
    bool               blocked; /* the body says why the board cannot be used instead */
    sn_mode_t          mode;
    const sn_screen_t *pending; /* the OFFENSIVE menu, while its question is up */
    const sn_screen_t *log;     /* what the alert line opens */
    lv_obj_t          *crumb, *purpose, *chip, *strip, *strip_tag, *strip_tag_text, *strip_text, *strip_cap, *body, *bar;
    lv_obj_t          *tile[TILE_MAX], *tile_line[TILE_MAX], *tile_mark[TILE_MAX], *tile_quote[TILE_MAX];
    action_t           act[ACT_MAX];
    int                nact;
    const char        *help;
    bool               strip_active;
    lv_timer_t        *timer;
    char               say[96];
    sn_level_t         say_level;
    uint32_t           say_until;
    sn_fn_t            confirm_yes;
    sentinel_state_t   st;
} s_fr DECK_PSRAM_BSS;

static void show(void);
static void tick(lv_timer_t *t);

static const sn_screen_t *top(void) { return s_fr.depth ? s_fr.stack[s_fr.depth - 1] : NULL; }

sn_mode_t sn_mode(void) { return s_fr.mode; }

/* Take the leaf on top down: its timers, and what it asked of the node. Always before the stack moves. */
static void leaf_down(void)
{
    const sn_screen_t *sc = top();
    if (s_fr.built && sc && sc->destroy) sc->destroy();
    s_fr.built = false;
}

/* ---- Levels ---------------------------------------------------------------- */

static bool has(const char *text, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = text; *p; p++)
        if (strncasecmp(p, word, n) == 0) return true;
    return false;
}

sn_level_t sn_level_of(bool warn, const char *text)
{
    if (!warn || !text) return SN_NOTE;
    /* One channel coming up or going quiet is a neighbour's weather station: worth a row, not a warning. */
    if (has(text, "MHz active") || has(text, "MHz quiet")) return SN_NOTE;
    if (has(text, "likely jam") || has(text, "deauth") || has(text, "karma") || has(text, "beacon flood") || has(text, "saturated") ||
        has(text, "band up"))
        return SN_ALERT;
    return SN_WARNING;
}

lv_color_t sn_level_color(sn_level_t level) { return level == SN_ALERT ? g_pal.danger : level == SN_WARNING ? g_pal.warn : g_pal.text; }

const char *sn_level_mark(sn_level_t level) { return level == SN_ALERT ? "!!" : level == SN_WARNING ? "!" : ""; }

/* ---- Plain things ---------------------------------------------------------- */

/* A container that is nothing but layout: not a control, so it takes no taps. */
static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t *sn_col(lv_obj_t *parent, int32_t width)
{
    lv_obj_t *c = plain(parent);
    lv_obj_set_height(c, LV_PCT(100));
    if (width > 0) lv_obj_set_width(c, width);
    else lv_obj_set_flex_grow(c, 1);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    return c;
}

lv_obj_t *sn_row_box(lv_obj_t *parent)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 24, 0);
    return r;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = label(parent, font, color, "");
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

lv_obj_t *sn_heading(lv_obj_t *parent, const char *text)
{
    lv_obj_t *box = plain(parent);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_style_margin_top(box, 6, 0);
    lv_obj_t *l    = label(box, g_font.mono_m, g_pal.label, text);
    lv_obj_t *rule = plain(box);
    lv_obj_set_size(rule, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(rule, g_pal.line, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    return l;
}

lv_obj_t *sn_state(lv_obj_t *parent) { return wrapped(parent, g_font.disp_l, g_pal.text); }
lv_obj_t *sn_value(lv_obj_t *parent) { return wrapped(parent, g_font.mono_b, g_pal.text); }
lv_obj_t *sn_note(lv_obj_t *parent) { return wrapped(parent, g_font.mono_m, g_pal.label); }

void sn_set(lv_obj_t *l, lv_color_t color, const char *fmt, ...)
{
    if (!l) return;
    char    buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(lv_label_get_text(l), buf) != 0) lv_label_set_text(l, buf);
    lv_obj_set_style_text_color(l, color, 0);
}

void sn_fmt_span(char *out, size_t n, uint32_t ms)
{
    unsigned s = (unsigned)(ms / 1000);
    if (s < 60) snprintf(out, n, "%u S", s);
    else if (s < 3600) snprintf(out, n, "%u MIN", s / 60);
    else snprintf(out, n, "%u H %u MIN", s / 3600, s / 60 % 60);
}

void sn_upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

const char *sn_board_problem(const sentinel_state_t *st)
{
    if (!sn_board_enabled() || !st->powered) return "THE RADIO BOARD IS OFF";
    if (!sentinel_link(st)) return "NO LINK TO THE RADIO BOARD";
    return NULL;
}

/* ---- Things that can be tapped --------------------------------------------- */

/* The one look every control has. `c` is its colour: the accent, or the
 * danger colour for something that transmits or ends something. */
static void control_style(lv_obj_t *b, lv_color_t c, int32_t radius)
{
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(c, g_pal.bg, 52), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, c, 0);
    lv_obj_set_style_bg_color(b, lv_color_mix(c, g_pal.bg, 150), LV_STATE_PRESSED);
    /* chosen with the keyboard: a thick white edge */
    lv_obj_set_style_border_width(b, 4, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, lv_color_mix(c, g_pal.bg, 96), LV_STATE_CHECKED);
    /* switched off: an outline and nothing in it */
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_border_color(b, g_pal.line, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, g_pal.dim, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
}

static lv_obj_t *control(lv_obj_t *parent, lv_color_t c, int32_t radius)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    control_style(b, c, radius);
    return b;
}

/* The key on a button: a solid block with the key's name in it. */
static lv_obj_t *keycap(lv_obj_t *parent, const char *key, lv_color_t c)
{
    lv_obj_t *k = label(parent, g_font.mono_m, g_pal.bg, key);
    lv_obj_set_style_bg_color(k, c, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, 5, 0);
    lv_obj_set_style_pad_hor(k, 8, 0);
    lv_obj_set_style_pad_ver(k, 3, 0);
    return k;
}

static void fn_clicked(lv_event_t *e)
{
    sn_fn_t fn = (sn_fn_t)lv_event_get_user_data(e);
    if (fn) fn();
}

lv_obj_t *sn_button(lv_obj_t *parent, const char *key, const char *text, sn_fn_t fn)
{
    lv_obj_t *b = control(parent, g_pal.accent, 10);
    lv_obj_set_size(b, LV_SIZE_CONTENT, BAR_H);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (key && key[0]) keycap(b, key, g_pal.accent);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, g_font.mono_b, 0);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, fn_clicked, LV_EVENT_CLICKED, (void *)fn);
    return b;
}

/* The label is the last child; a key cap, when there is one, the first. */
static lv_obj_t *button_label(lv_obj_t *b) { return lv_obj_get_child(b, (int32_t)lv_obj_get_child_count(b) - 1); }

void sn_button_label(lv_obj_t *b, const char *text)
{
    lv_obj_t *l = b ? button_label(b) : NULL;
    if (l && strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

void sn_button_danger(lv_obj_t *b, bool on)
{
    if (!b) return;
    lv_color_t c = on ? g_pal.danger : g_pal.accent;
    control_style(b, c, 10);
    if (lv_obj_get_child_count(b) > 1) lv_obj_set_style_bg_color(lv_obj_get_child(b, 0), c, 0);
}

/* BACK points the way it goes: its left end is an arrow head, not a corner. */
static void back_draw(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);
    lv_color_t c = g_pal.accent, bg = lv_color_mix(c, g_pal.bg, lv_obj_has_state(obj, LV_STATE_PRESSED) ? 150 : 52);
    int32_t    d = lv_area_get_height(&a) / 2, ym = (a.y1 + a.y2) / 2;
    int32_t    x0 = a.x1 + 2, x1 = a.x1 + d, x2 = a.x2 - 1, y1 = a.y1 + 1, y2 = a.y2 - 1;

    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = bg;
    t.opa   = LV_OPA_COVER;
    t.p[0]  = (lv_point_precise_t){x0, ym};
    t.p[1]  = (lv_point_precise_t){x1, y1};
    t.p[2]  = (lv_point_precise_t){x1, y2};
    lv_draw_triangle(layer, &t);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color   = bg;
    r.bg_opa     = LV_OPA_COVER;
    lv_area_t body = {x1, y1, x2, y2};
    lv_draw_rect(layer, &r, &body);

    const lv_point_precise_t pt[6] = {{x0, ym}, {x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}, {x0, ym}};
    for (int i = 0; i < 5; i++) {
        lv_draw_line_dsc_t l;
        lv_draw_line_dsc_init(&l);
        l.color       = c;
        l.width       = 2;
        l.round_start = l.round_end = 1;
        l.p1 = pt[i], l.p2 = pt[i + 1];
        lv_draw_line(layer, &l);
    }
}

static void redraw(lv_event_t *e) { lv_obj_invalidate(lv_event_get_target_obj(e)); }

static lv_obj_t *back_button(lv_obj_t *parent, const char *text, sn_fn_t fn)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, LV_SIZE_CONTENT, BAR_H);
    lv_obj_set_style_pad_left(b, BAR_H / 2 + 8, 0); /* the text starts clear of the arrow head */
    lv_obj_set_style_pad_right(b, 18, 0);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    keycap(b, "ESC", g_pal.accent);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, g_font.mono_b, 0);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, back_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(b, redraw, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, redraw, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(b, redraw, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(b, fn_clicked, LV_EVENT_CLICKED, (void *)fn);
    return b;
}

/* ---- Actions --------------------------------------------------------------- */

static action_t *action_of(sn_fn_t fn)
{
    for (int i = 0; i < s_fr.nact; i++)
        if (s_fr.act[i].fn == fn) return &s_fr.act[i];
    return NULL;
}

void sn_action(uint8_t hid, const char *key, const char *text, sn_fn_t fn)
{
    if (!s_fr.bar || s_fr.nact >= ACT_MAX) return;
    action_t *a = &s_fr.act[s_fr.nact++];
    a->hid      = hid;
    a->fn       = fn;
    a->enabled  = true;
    a->danger   = false;
    a->btn      = fn == sn_back ? back_button(s_fr.bar, text, fn) : sn_button(s_fr.bar, key, text, fn);
    a->label    = lv_obj_get_child(a->btn, key && key[0] ? 1 : 0);
}

void sn_action_label(sn_fn_t fn, const char *text)
{
    action_t *a = action_of(fn);
    if (a && strcmp(lv_label_get_text(a->label), text) != 0) lv_label_set_text(a->label, text);
}

void sn_action_enable(sn_fn_t fn, bool on)
{
    action_t *a = action_of(fn);
    if (!a || a->enabled == on) return;
    a->enabled = on;
    if (on) lv_obj_remove_state(a->btn, LV_STATE_DISABLED);
    else lv_obj_add_state(a->btn, LV_STATE_DISABLED);
    /* the key on it goes grey too: a bright key on a dead button reads as live */
    if (lv_obj_get_child_count(a->btn) > 1) lv_obj_set_style_bg_color(lv_obj_get_child(a->btn, 0), on ? (a->danger ? g_pal.danger : g_pal.accent) : g_pal.line, 0);
}

void sn_action_danger(sn_fn_t fn, bool on)
{
    action_t *a = action_of(fn);
    if (!a || a->danger == on) return;
    a->danger = on;
    lv_color_t c = on ? g_pal.danger : g_pal.accent;
    control_style(a->btn, c, 10);
    if (lv_obj_get_child_count(a->btn) > 1) lv_obj_set_style_bg_color(lv_obj_get_child(a->btn, 0), a->enabled ? c : g_pal.line, 0);
}

static void help_show(void)
{
    if (!s_fr.help || !top()) return;
    const char *cols[1] = {s_fr.help};
    deck_modal_info(top()->name, cols, 1);
}

void sn_help(const char *text)
{
    s_fr.help = text;
    sn_action(HID_H, "H", "HELP", help_show);
}

static void confirm_answer(bool yes, void *user)
{
    (void)user;
    sn_fn_t fn        = s_fr.confirm_yes;
    s_fr.confirm_yes = NULL;
    if (yes && fn && s_fr.parent) fn();
}

void sn_confirm(const char *title, const char *message, const char *yes_label, sn_fn_t yes)
{
    s_fr.confirm_yes = yes;
    deck_modal_confirm_danger(title, message, yes_label, confirm_answer, NULL);
}

/* ---- Lists ----------------------------------------------------------------- */

static void row_clicked(lv_event_t *e)
{
    sn_list_t *l = lv_event_get_user_data(e);
    lv_obj_t  *t = lv_event_get_target_obj(e);
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        if (l->row[r] != t) continue;
        int k = sn_list_item(l, r);
        if (k < 0) return;
        l->sel = k;
        if (l->open) l->open(k);
        return;
    }
}

void sn_list_build(sn_list_t *l, lv_obj_t *parent, void (*open)(int index))
{
    memset(l, 0, sizeof(*l));
    l->open = open;
    l->box  = plain(parent);
    lv_obj_set_width(l->box, LV_PCT(100));
    lv_obj_set_flex_grow(l->box, 1);
    lv_obj_set_flex_flow(l->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l->box, 6, 0);
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        lv_obj_t *row = control(l->box, g_pal.accent, 8);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_mix(g_pal.accent, g_pal.bg, 120), 0);
        lv_obj_set_style_bg_color(row, lv_color_mix(g_pal.accent, g_pal.bg, 24), 0);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *t = label(row, g_font.mono_b, g_pal.text, "");
        lv_obj_set_flex_grow(t, 1);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(t, lv_font_get_line_height(g_font.mono_b));
        label(row, g_font.mono_b, g_pal.accent, ">"); /* it opens */
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, l);
        l->row[r]  = row;
        l->text[r] = t;
    }
    l->more = label(l->box, g_font.mono_m, g_pal.label, "");
}

void sn_list_set(sn_list_t *l, int n, int sel, const char *empty)
{
    if (sel >= n) sel = n - 1;
    if (sel < 0) sel = 0;
    int top_row = sel - SN_LIST_ROWS / 2;
    if (top_row > n - SN_LIST_ROWS) top_row = n - SN_LIST_ROWS;
    if (top_row < 0) top_row = 0;
    l->n = n, l->sel = sel, l->top = top_row;
    for (int r = 0; r < SN_LIST_ROWS; r++) {
        int k = top_row + r;
        if (k < n) {
            lv_obj_remove_flag(l->row[r], LV_OBJ_FLAG_HIDDEN);
            if (k == sel) lv_obj_add_state(l->row[r], LV_STATE_CHECKED);
            else lv_obj_remove_state(l->row[r], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(l->row[r], LV_OBJ_FLAG_HIDDEN);
        }
    }
    char buf[96];
    if (n == 0) snprintf(buf, sizeof(buf), "%s", empty ? empty : "NOTHING YET");
    else if (n > SN_LIST_ROWS) snprintf(buf, sizeof(buf), "%d TO %d OF %d      UP AND DOWN MOVE, ENTER OPENS", top_row + 1, top_row + SN_LIST_ROWS, n);
    else snprintf(buf, sizeof(buf), "%d IN ALL      UP AND DOWN MOVE, ENTER OPENS", n);
    if (strcmp(lv_label_get_text(l->more), buf) != 0) lv_label_set_text(l->more, buf);
}

int sn_list_item(const sn_list_t *l, int row)
{
    int k = l->top + row;
    return row >= 0 && row < SN_LIST_ROWS && k < l->n ? k : -1;
}

lv_obj_t *sn_list_text(const sn_list_t *l, int row) { return l->text[row]; }

bool sn_list_key(sn_list_t *l, const deck_key_t *k)
{
    if (!l->box) return false;
    if (k->code == HID_UP || k->code == HID_DOWN) {
        int s = l->sel + (k->code == HID_DOWN ? 1 : -1);
        l->sel = s < 0 ? 0 : s >= l->n ? (l->n ? l->n - 1 : 0) : s;
        return true;
    }
    if (k->code == HID_ENTER && l->n > 0) {
        if (l->open) l->open(l->sel);
        return true;
    }
    return false;
}

/* ---- The mode -------------------------------------------------------------- */

static void end_offense(const char *why)
{
    if (s_fr.mode != SN_OFFENSIVE) return;
    sentinel_arm(false);
    sentinel_note(false, why);
    deck_notice_set_flag(NULL);
    s_fr.mode = SN_DEFENSIVE;
}

static void apply_mode(void)
{
    bool off = false;
    for (int i = 0; i < s_fr.depth; i++)
        if (s_fr.stack[i]->flags & SN_OFFENSE) off = true;
    if (off && s_fr.mode == SN_DEFENSIVE) {
        s_fr.mode = SN_OFFENSIVE;
        sentinel_arm(true);
        sentinel_note(false, "offensive mode entered");
        deck_notice_set_flag("OFFENSIVE");
    } else if (!off) {
        end_offense("offensive mode left");
    }
}

static void offense_answer(bool yes, void *user)
{
    (void)user;
    const sn_screen_t *sc = s_fr.pending;
    s_fr.pending          = NULL;
    if (!yes || !sc || !s_fr.parent || s_fr.depth >= STACK_MAX) return;
    s_fr.stack[s_fr.depth] = sc;
    s_fr.sel[s_fr.depth]   = 0;
    s_fr.depth++;
    show();
}

/* ---- Navigation ------------------------------------------------------------ */

void sn_open(const sn_screen_t *sc)
{
    if (!sc || !s_fr.parent || s_fr.depth >= STACK_MAX) return;
    if ((sc->flags & SN_OFFENSE) && s_fr.mode == SN_DEFENSIVE) {
        s_fr.pending = sc;
        deck_modal_confirm_danger("ENTER OFFENSIVE MODE?",
                                  "The tools in here transmit. Use them only on equipment you own, or with the owner's full "
                                  "permission and knowledge.\n\nLeaving this area, or the screen sleeping, returns to DEFENSIVE.",
                                  "ENTER OFFENSIVE", offense_answer, NULL);
        return;
    }
    leaf_down();
    s_fr.stack[s_fr.depth] = sc;
    s_fr.sel[s_fr.depth]   = 0;
    s_fr.depth++;
    show();
}

/* True when BACK was used inside SENTINEL; false at the home page. */
static bool pop(void)
{
    const sn_screen_t *sc = top();
    if (sc && !sc->kids && s_fr.built && sc->back && sc->back()) return true;
    if (s_fr.depth <= 1) return false;
    leaf_down();
    s_fr.depth--;
    show();
    return true;
}

void sn_back(void)
{
    if (!pop()) deck_shell_back(); /* on_back says no at the home page, so the shell leaves */
}

void sn_refresh(void)
{
    if (!s_fr.parent || !s_fr.depth) return;
    leaf_down();
    show();
}

/* ---- Menu pages ------------------------------------------------------------ */

static void tile_select(int n, int sel)
{
    for (int i = 0; i < n && i < TILE_MAX; i++) {
        if (!s_fr.tile[i]) continue;
        if (i == sel) lv_obj_add_state(s_fr.tile[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_fr.tile[i], LV_STATE_CHECKED);
    }
}

static void tile_open(int i)
{
    const sn_screen_t *m = top();
    if (!m || !m->kids || i < 0 || i >= m->nkids) return;
    s_fr.sel[s_fr.depth - 1] = i;
    tile_select(m->nkids, i);
    sn_open(m->kids[i]);
}

static void tile_clicked(lv_event_t *e) { tile_open((int)(intptr_t)lv_event_get_user_data(e)); }

/* Under a tile's name: what its area is doing now, if it has something to
 * say, then its line from a film. A tile with neither says what it is for. */
static void tile_status(int i, lv_color_t color, const char *text)
{
    lv_obj_t *l = s_fr.tile_line[i];
    if (text[0]) lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    sn_set(l, color, "%s", text);
}

static void tiles_update(void)
{
    const sn_screen_t *m = top();
    if (!m || !m->kids) return;
    const char *problem = sn_board_problem(&s_fr.st);
    for (int i = 0; i < m->nkids && i < TILE_MAX; i++) {
        const sn_screen_t *k = m->kids[i];
        char               line[96] = "";
        sn_level_t         lvl = SN_NOTE;
        if (problem && (k->flags & SN_BOARD)) {
            tile_status(i, g_pal.warn, problem);
            lv_label_set_text(s_fr.tile_mark[i], "");
            continue;
        }
        if (k->status) k->status(&s_fr.st, line, sizeof(line), &lvl);
        else if (!k->quote) snprintf(line, sizeof(line), "%s", k->purpose ? k->purpose : "");
        tile_status(i, lvl == SN_NOTE ? lv_color_white() : sn_level_color(lvl), line);
        sn_set(s_fr.tile_mark[i], sn_level_color(lvl), "%s", sn_level_mark(lvl));
    }
}

static void menu_build(const sn_screen_t *m)
{
    int n    = m->nkids > TILE_MAX ? TILE_MAX : m->nkids;
    int cols = n <= 4 ? n : n <= 6 ? 3 : 4, rows = (n + cols - 1) / cols;
    lv_obj_update_layout(s_fr.body);
    int32_t bw = lv_obj_get_content_width(s_fr.body), bh = lv_obj_get_content_height(s_fr.body), gap = 14;
    int32_t w = (bw - gap * (cols - 1)) / cols, h = (bh - gap * (rows - 1)) / rows;
    if (w > 420) w = 420;
    if (h > 240) h = 240;
    lv_obj_set_flex_flow(s_fr.body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_fr.body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_fr.body, gap, 0);
    lv_obj_set_style_pad_row(s_fr.body, gap, 0);
    for (int i = 0; i < n; i++) {
        const sn_screen_t *k = m->kids[i];
        lv_color_t         c = (k->flags & SN_OFFENSE) ? g_pal.danger : g_pal.accent;
        lv_obj_t          *t = control(s_fr.body, c, 12);
        lv_obj_set_size(t, w, h);
        lv_obj_set_style_pad_all(t, 16, 0);
        lv_obj_set_style_pad_row(t, 10, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        /* The same rise of colour from the top edge that the DECK//OS home tiles have. */
        lv_obj_set_style_bg_color(t, lv_color_mix(c, g_pal.bg, 118), 0);
        lv_obj_set_style_bg_grad_color(t, lv_color_mix(c, g_pal.bg, 20), 0);
        lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_grad_stop(t, 175, 0);
        lv_obj_set_style_bg_color(t, lv_color_mix(c, g_pal.bg, 190), LV_STATE_CHECKED);
        lv_obj_set_style_bg_grad_color(t, lv_color_mix(c, g_pal.bg, 70), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(t, lv_color_mix(c, g_pal.bg, 220), LV_STATE_PRESSED);
        lv_obj_set_style_clip_corner(t, true, 0);
        /* Its icon again, huge and faint, behind everything else on the tile. */
        if (k->icon) {
            lv_obj_t *ghost = label(t, g_font.icon_xl, c, k->icon);
            lv_obj_add_flag(ghost, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_style_text_opa(ghost, 34, 0);
            lv_obj_align(ghost, LV_ALIGN_RIGHT_MID, 24, 12);
        }
        lv_obj_t *head = sn_row_box(t);
        lv_obj_set_style_pad_column(head, 12, 0);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        keycap(head, num, c);
        lv_obj_t *name = label(head, g_font.disp_m, lv_color_white(), k->name);
        lv_obj_set_flex_grow(name, 1);
        s_fr.tile_mark[i] = label(head, g_font.disp_m, g_pal.warn, "");
        s_fr.tile_line[i]  = wrapped(t, g_font.mono_m, lv_color_white());
        /* The film line is set apart twice: it sits at the foot of the tile, and it is in the tile's own colour. */
        lv_obj_t *gap = plain(t);
        lv_obj_set_flex_grow(gap, 1);
        s_fr.tile_quote[i] = wrapped(t, g_font.mono_m, lv_color_mix(c, lv_color_white(), 150));
        if (k->quote) lv_label_set_text(s_fr.tile_quote[i], k->quote);
        else lv_obj_add_flag(s_fr.tile_quote[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(t, tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_fr.tile[i] = t;
    }
    tile_select(n, s_fr.sel[s_fr.depth - 1]);
    tiles_update();
}

static bool menu_key(const sn_screen_t *m, const deck_key_t *k)
{
    int n    = m->nkids > TILE_MAX ? TILE_MAX : m->nkids;
    int cols = n <= 4 ? n : n <= 6 ? 3 : 4;
    int s    = s_fr.sel[s_fr.depth - 1];
    switch (k->code) {
        case HID_LEFT: s = (s + n - 1) % n; break;
        case HID_RIGHT:
        case HID_TAB: s = (s + 1) % n; break;
        case HID_UP:
            if (s - cols >= 0) s -= cols;
            break;
        case HID_DOWN:
            if (s + cols < n) s += cols;
            break;
        case HID_ENTER:
        case HID_SPACE: tile_open(s); return true;
        default:
            if (k->code >= HID_1 && k->code < HID_1 + n) {
                tile_open(k->code - HID_1);
                return true;
            }
            return false;
    }
    s_fr.sel[s_fr.depth - 1] = s;
    tile_select(n, s);
    return true;
}

/* ---- The alert line -------------------------------------------------------- */

static void open_log_now(void)
{
    if (s_fr.log && top() != s_fr.log) sn_open(s_fr.log);
}

/* The leaf may hold the way out (a drive, a capture) and open LOG itself once the user agrees. */
static void strip_open(void)
{
    if (!s_fr.strip_active || !s_fr.log || top() == s_fr.log) return;
    const sn_screen_t *sc = top();
    if (sc && !sc->kids && s_fr.built && sc->hold && sc->hold(open_log_now)) return;
    open_log_now();
}

static void strip_clicked(lv_event_t *e)
{
    (void)e;
    strip_open();
}

/* The bar along the bottom, built like the info bar on the DECK//OS home
 * screen: a chamfered panel with a filled tag at its left end. The tag says
 * what kind of line it is, so the level is a word as well as a colour. */
static void strip_set(sn_level_t level, bool active, const char *text)
{
    if (!s_fr.strip) return;
    lv_color_t c = active ? sn_level_color(level) : level == SN_NOTE ? g_pal.live : g_pal.warn;
    s_fr.strip_active = active && s_fr.log && top() != s_fr.log;
    deck_panel_set_outline(s_fr.strip, active ? c : g_pal.line);
    deck_panel_set_fill(s_fr.strip_tag, c);
    deck_panel_set_outline(s_fr.strip_tag, c);
    sn_set(s_fr.strip_tag_text, g_pal.bg, "%s", !active ? "SENTINEL>" : level == SN_ALERT ? "ALERT>" : "WARNING>");
    lv_obj_set_style_bg_color(s_fr.strip_cap, c, 0);
    if (s_fr.strip_active) {
        lv_obj_add_flag(s_fr.strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_fr.strip_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_fr.strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_fr.strip_cap, LV_OBJ_FLAG_HIDDEN);
    }
    sn_set(s_fr.strip_text, active ? lv_color_white() : level == SN_NOTE ? g_pal.text : c, "%s", text);
}

static void strip_update(void)
{
    if (!s_fr.strip) return;
    const sentinel_state_t *st  = &s_fr.st;
    uint32_t                now = sentinel_now_ms();
    char                    text[200], span[24];
    const sentinel_event_t *best = NULL, *last = NULL;
    sn_level_t              bl   = SN_NOTE;
    for (int i = 0; i < st->evt_count; i++) {
        const sentinel_event_t *e = sentinel_event(st, i);
        if (!e) break;
        sn_level_t l = sn_level_of(e->warn, e->text);
        if (l == SN_NOTE) continue;
        if (!last) last = e;
        if (now - e->ms < (l == SN_ALERT ? ALERT_HOLD_MS : WARN_HOLD_MS) && l > bl) best = e, bl = l;
    }
    bool band_up = st->nrf_band == SENTINEL_BAND_UP || st->sub_band == SENTINEL_BAND_UP;
    /* What the deck itself has to say (a survey saved, a baseline reset) never covers an alert that is live now. */
    if (s_fr.say[0] && (int32_t)(s_fr.say_until - now) > 0 && bl < SN_ALERT && !band_up) {
        strip_set(s_fr.say_level, false, s_fr.say);
        return;
    }
    s_fr.say[0] = 0;
    const char *problem = sn_board_problem(st);
    if (problem) {
        snprintf(text, sizeof(text), "%s: NOTHING ON THE AIR IS BEING WATCHED", problem);
        strip_set(SN_WARNING, false, text);
        return;
    }
    /* A band that is up now is an alert for as long as it lasts, whatever the events' ages. */
    if (bl < SN_ALERT && st->nrf_band == SENTINEL_BAND_UP) {
        snprintf(text, sizeof(text), "2.4 GHZ BAND SATURATED: %u CHANNELS SOLID, NOW", (unsigned)st->nrf_band_ch);
        strip_set(SN_ALERT, true, text);
    } else if (bl < SN_ALERT && st->sub_band == SENTINEL_BAND_UP) {
        snprintf(text, sizeof(text), "433 MHZ BAND UP %d DB ACROSS %u CHANNELS, NOW", (int)st->sub_band_db, (unsigned)st->sub_band_ch);
        strip_set(SN_ALERT, true, text);
    } else if (best) {
        sn_fmt_span(span, sizeof(span), now - best->ms);
        snprintf(text, sizeof(text), "%s   (%s AGO)", best->text, span);
        strip_set(bl, true, text);
    } else if (last) {
        sn_fmt_span(span, sizeof(span), now - last->ms);
        snprintf(text, sizeof(text), "ALL CLEAR.  LAST WARNING %s AGO: %s", span, last->text);
        strip_set(SN_NOTE, false, text);
    } else {
        strip_set(SN_NOTE, false, "ALL CLEAR.  NO WARNINGS SINCE SENTINEL STARTED.");
    }
}

/* ---- Building a screen ------------------------------------------------------ */

static void strip_build(lv_obj_t *root)
{
    s_fr.strip = deck_panel(root, DECK_CUT_TL | DECK_CUT_BR, 12);
    lv_obj_set_size(s_fr.strip, LV_PCT(100), STRIP_H);
    lv_obj_set_style_pad_hor(s_fr.strip, 14, 0);
    lv_obj_set_style_pad_column(s_fr.strip, 14, 0);
    lv_obj_set_flex_flow(s_fr.strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_fr.strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_fr.strip_tag = deck_panel(s_fr.strip, DECK_CUT_BR, 8);
    lv_obj_set_size(s_fr.strip_tag, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_pad_hor(s_fr.strip_tag, 10, 0);
    s_fr.strip_tag_text = label(s_fr.strip_tag, g_font.mono_m, g_pal.bg, "SENTINEL>");
    lv_obj_center(s_fr.strip_tag_text);
    s_fr.strip_text = label(s_fr.strip, g_font.mono_b, g_pal.text, "");
    lv_obj_set_flex_grow(s_fr.strip_text, 1);
    lv_label_set_long_mode(s_fr.strip_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_fr.strip_text, lv_font_get_line_height(g_font.mono_b));
    s_fr.strip_cap = keycap(s_fr.strip, "L  OPEN LOG", g_pal.warn);
    lv_obj_add_event_cb(s_fr.strip, strip_clicked, LV_EVENT_CLICKED, NULL);
}


static void forget_widgets(void)
{
    s_fr.crumb = s_fr.purpose = s_fr.chip = s_fr.strip = s_fr.strip_tag = s_fr.strip_tag_text = s_fr.strip_text = s_fr.strip_cap = s_fr.body = s_fr.bar = NULL;
    memset(s_fr.tile, 0, sizeof(s_fr.tile));
    memset(s_fr.tile_line, 0, sizeof(s_fr.tile_line));
    memset(s_fr.tile_mark, 0, sizeof(s_fr.tile_mark));
    memset(s_fr.tile_quote, 0, sizeof(s_fr.tile_quote));
    memset(s_fr.act, 0, sizeof(s_fr.act));
    s_fr.nact = 0;
    s_fr.help = NULL;
}

static void blocked_build(const char *problem)
{
    lv_obj_set_flex_align(s_fr.body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *s = label(s_fr.body, g_font.disp_l, g_pal.warn, problem);
    (void)s;
    label(s_fr.body, g_font.mono_b, g_pal.text,
          sn_board_enabled() ? "This screen needs the radio board. It is powered and has not answered yet."
                             : "This screen needs the radio board. Switch it on in BOARD > RADIO BOARD.");
}

lv_obj_t *sn_body_reset(void)
{
    if (!s_fr.body) return NULL;
    lv_obj_clean(s_fr.body);
    /* every action but BACK goes with the page */
    for (int i = 1; i < s_fr.nact; i++)
        if (s_fr.act[i].btn) lv_obj_delete(s_fr.act[i].btn);
    if (s_fr.nact > 1) memset(&s_fr.act[1], 0, sizeof(action_t) * (size_t)(s_fr.nact - 1));
    if (s_fr.nact > 1) s_fr.nact = 1;
    s_fr.help = NULL;
    return s_fr.body;
}

static void show(void)
{
    /* The caller has already taken the old leaf down (leaf_down), while it was still on top of the stack. */
    lv_group_remove_all_objs(deck_input_group());
    lv_obj_clean(s_fr.parent);
    forget_widgets();
    apply_mode();
    const sn_screen_t *sc = top();
    if (!sc) return;

    lv_obj_t *root = plain(s_fr.parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(root, 14, 0);
    lv_obj_set_style_pad_ver(root, 10, 0);
    lv_obj_set_style_pad_row(root, 8, 0);

    /* 1. Where you are and why. */
    lv_obj_t *head = sn_row_box(root);
    lv_obj_t *left = plain(head);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 2, 0);
    char crumb[128];
    int  o = 0;
    for (int i = 0; i < s_fr.depth && o < (int)sizeof(crumb) - 1; i++)
        o += snprintf(crumb + o, sizeof(crumb) - (size_t)o, "%s%s", i ? "  >  " : "", s_fr.stack[i]->name);
    s_fr.crumb   = label(left, g_font.disp_m, lv_color_white(), crumb);
    s_fr.purpose = label(left, g_font.mono_m, g_pal.label, sc->purpose ? sc->purpose : "");
    lv_obj_set_width(s_fr.purpose, LV_PCT(100));
    lv_label_set_long_mode(s_fr.purpose, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_fr.purpose, lv_font_get_line_height(g_font.mono_m));
    /* Started without a log there is no board behind the frame (NET TOOLS): no mode, no alert line. */
    bool off  = s_fr.mode == SN_OFFENSIVE;
    s_fr.chip = s_fr.log ? label(head, g_font.mono_b, off ? g_pal.danger : g_pal.ok, off ? "MODE: OFFENSIVE" : "MODE: DEFENSIVE") : NULL;

    /* 2. The body. */
    s_fr.body = plain(root);
    lv_obj_set_width(s_fr.body, LV_PCT(100));
    lv_obj_set_flex_grow(s_fr.body, 1);
    lv_obj_set_flex_flow(s_fr.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_fr.body, 8, 0);

    /* 3. The action bar. BACK is always its first button. */
    s_fr.bar = plain(root);
    lv_obj_set_size(s_fr.bar, LV_PCT(100), BAR_H);
    lv_obj_set_flex_flow(s_fr.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_fr.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_fr.bar, 10, 0);
    char back[40]; /* where it goes: LEAVE SENTINEL at the home page, BACK TO WI-FI below it */
    if (s_fr.depth > 1) snprintf(back, sizeof(back), "BACK TO %s", s_fr.stack[s_fr.depth - 2]->name);
    else snprintf(back, sizeof(back), "LEAVE %s", s_fr.stack[0]->name);
    sn_action(0, "ESC", back, sn_back);

    /* 4. The alert line, along the bottom edge: the same bar the DECK//OS home screen has there. */
    if (s_fr.log) strip_build(root);


    sentinel_snapshot(&s_fr.st);
    const char *problem = (sc->flags & SN_BOARD) && !(sc->flags & SN_KEEP_TOOL) ? sn_board_problem(&s_fr.st) : NULL;
    s_fr.blocked        = false;
    if (sc->kids) {
        menu_build(sc);
        if (s_fr.depth == 1) {
            lv_obj_t *hint = label(s_fr.bar, g_font.mono_m, g_pal.label, "ARROWS CHOOSE, ENTER OPENS, OR PRESS A TILE'S NUMBER");
            lv_obj_set_style_margin_left(hint, 16, 0);
        }
    } else if (problem) {
        s_fr.blocked = true;
        blocked_build(problem);
    } else if (sc->build) {
        s_fr.built = true;
        sc->build(s_fr.body);
    }
    strip_update();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    const sn_screen_t *sc = top();
    if (!sc || !s_fr.body) return;
    sentinel_snapshot(&s_fr.st);
    /* The board came or went under a screen that needs it. */
    if (!sc->kids && (sc->flags & SN_BOARD) && !(sc->flags & SN_KEEP_TOOL)) {
        bool problem = sn_board_problem(&s_fr.st) != NULL;
        if (problem != s_fr.blocked) {
            leaf_down();
            show();
            return;
        }
    }
    strip_update();
    tiles_update();
}

/* ---- What the app module calls ---------------------------------------------- */

void sn_frame_start(lv_obj_t *parent, const sn_screen_t *home, const sn_screen_t *log)
{
    sn_mode_t mode = s_fr.mode; /* DEFENSIVE: the app's exit ended anything else */
    memset(&s_fr, 0, sizeof(s_fr));
    s_fr.mode     = mode;
    s_fr.parent   = parent;
    s_fr.log      = log;
    s_fr.stack[0] = home;
    s_fr.depth    = 1;
    show();
    s_fr.timer = lv_timer_create(tick, TICK_MS, NULL);
}

void sn_frame_exit(const char *why) { end_offense(why); }

void sn_frame_stop(void)
{
    if (s_fr.timer) lv_timer_delete(s_fr.timer);
    leaf_down(); /* the widgets are already gone: a destroy only stops timers and hands the radio back */
    sn_mode_t mode = s_fr.mode;
    memset(&s_fr, 0, sizeof(s_fr));
    s_fr.mode = mode;
}

void sn_say(sn_level_t level, const char *text, uint32_t ms)
{
    snprintf(s_fr.say, sizeof(s_fr.say), "%s", text);
    s_fr.say_level = level;
    s_fr.say_until = sentinel_now_ms() + ms;
}

/* The screen slept with OFFENSIVE on: back to the home page, and say so. */
void sn_frame_offense_slept(void)
{
    if (s_fr.mode != SN_OFFENSIVE) return;
    if (s_fr.parent && s_fr.depth > 1) {
        leaf_down();
        s_fr.depth = 1;
        show(); /* ends the mode: nothing under OFFENSIVE is open any more */
    } else {
        end_offense("offensive mode ended: screen slept");
    }
    sn_say(SN_WARNING, "OFFENSIVE MODE ENDED WHEN THE SCREEN SLEPT. NOTHING IS TRANSMITTING.", 60000);
}

bool sn_frame_back(void) { return pop(); }

static bool typing(void)
{
    lv_obj_t *f = lv_group_get_focused(deck_input_group());
    return f && lv_obj_check_type(f, &lv_textarea_class);
}

bool sn_frame_key(const deck_key_t *k)
{
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    if (k->code == HID_ESC) return false; /* the shell's back, which asks on_back */
    const sn_screen_t *sc = top();
    if (!sc) return false;
    bool in_text = typing();
    if (!in_text && s_fr.strip_active && k->code == HID_LETTER('L')) {
        strip_open();
        return true;
    }
    if (sc->kids) return menu_key(sc, k);
    if (s_fr.blocked) return false;
    for (int i = 0; i < s_fr.nact; i++) {
        action_t *a = &s_fr.act[i];
        if (!a->hid || a->hid != k->code || !a->fn) continue;
        if (in_text && k->code != HID_ENTER) continue; /* letters belong to the field being typed in */
        if (a->enabled) a->fn();
        return true;
    }
    return sc->key ? sc->key(k) : false;
}
