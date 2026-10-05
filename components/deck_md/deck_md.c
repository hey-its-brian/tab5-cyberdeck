/*
 * md4c -> LVGL. md4c reports a stream of enter/leave block, enter/leave span
 * and text events; we keep a stack of container objects for blocks and a
 * "current" spangroup that receives inline text. Adjacent text with the same
 * inline style is merged into one span so long paragraphs stay cheap.
 */
#include "deck_md.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deck_theme.h"
#include "deck_widgets.h"
#include "md4c.h"

#define MAX_DEPTH 24
#define MAX_LISTS 8

typedef struct {
    bool ordered;
    unsigned next;
} list_t;

typedef struct {
    lv_obj_t *stack[MAX_DEPTH];
    int depth;

    lv_obj_t *spans;            /* spangroup receiving inline text, or NULL */
    const lv_font_t *font;      /* base style of the current text block */
    lv_color_t color;

    int strong, em, code, link, del;

    list_t lists[MAX_LISTS];
    int list_depth;

    bool in_code_block;
    bool code_is_html;
    lv_obj_t *code_label;

    int table_cols;
    bool in_thead;

    /* Pending text and the style it was collected under. */
    char *buf;
    size_t len, cap;
    int style_key;

    int blocks;
} ctx_t;

/* ---- Containers ---------------------------------------------------------- */

static lv_obj_t *top(ctx_t *c) { return c->stack[c->depth - 1]; }

static void push(ctx_t *c, lv_obj_t *o)
{
    if (c->depth < MAX_DEPTH) c->stack[c->depth++] = o;
}

static void pop(ctx_t *c)
{
    if (c->depth > 1) c->depth--;
}

static lv_obj_t *column(lv_obj_t *parent, int32_t gap)
{
    lv_obj_t *o = deck_box(parent);
    lv_obj_set_width(o, LV_PCT(100));
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

/* ---- Inline text --------------------------------------------------------- */

/* A small integer that identifies the visual style of a span. */
static int style_key(const ctx_t *c)
{
    if (c->code) return 1;
    if (c->link) return 2;
    if (c->del) return 3;
    if (c->strong) return 4;
    if (c->em) return 5;
    return 0;
}

static void flush(ctx_t *c)
{
    if (c->len == 0 || c->spans == NULL) {
        c->len = 0;
        return;
    }
    c->buf[c->len] = '\0';

    lv_span_t *sp     = lv_spangroup_add_span(c->spans);
    lv_style_t *st    = lv_span_get_style(sp);
    lv_color_t color  = c->color;
    const lv_font_t *font = c->font;
    switch (c->style_key) {
        case 1:
            color = g_pal.ok;
            font  = g_font.mono_m;
            break;
        case 2:
            color = g_pal.accent;
            lv_style_set_text_decor(st, LV_TEXT_DECOR_UNDERLINE);
            break;
        case 3:
            color = g_pal.dim;
            lv_style_set_text_decor(st, LV_TEXT_DECOR_STRIKETHROUGH);
            break;
        case 4: color = g_pal.accent; break;
        case 5: color = g_pal.accent2; break;
        default: break;
    }
    lv_style_set_text_color(st, color);
    lv_style_set_text_font(st, font);
    lv_span_set_text(sp, c->buf);
    c->len = 0;
}

static void append(ctx_t *c, const char *s, size_t n)
{
    int key = style_key(c);
    if (key != c->style_key) {
        flush(c);
        c->style_key = key;
    }
    if (c->len + n + 1 > c->cap) {
        size_t cap = (c->cap ? c->cap * 2 : 256);
        while (cap < c->len + n + 1) cap *= 2;
        char *nb = realloc(c->buf, cap);
        if (nb == NULL) return;
        c->buf = nb;
        c->cap = cap;
    }
    memcpy(c->buf + c->len, s, n);
    c->len += n;
}

static void text_block(ctx_t *c, lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    flush(c);
    c->spans = lv_spangroup_create(parent);
    lv_obj_set_width(c->spans, LV_PCT(100));
    lv_obj_set_style_text_line_space(c->spans, 4, 0);
    c->font      = font;
    c->color     = color;
    c->style_key = -1;
}

static void end_text_block(ctx_t *c)
{
    flush(c);
    if (c->spans) lv_spangroup_refresh(c->spans);
    c->spans = NULL;
}

/* md4c leaves entities undecoded; handle the common ones. */
static void append_entity(ctx_t *c, const char *s, size_t n)
{
    static const struct {
        const char *name;
        const char *utf8;
    } known[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
                 {"&nbsp;", " "}, {"&copy;", "(c)"}, {"&mdash;", "--"}, {"&ndash;", "-"}};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strlen(known[i].name) == n && memcmp(known[i].name, s, n) == 0) {
            append(c, known[i].utf8, strlen(known[i].utf8));
            return;
        }
    }
    if (n > 3 && s[1] == '#') {
        unsigned long cp = (s[2] == 'x' || s[2] == 'X') ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
        char u[4];
        size_t k = 0;
        if (cp < 0x80) {
            u[k++] = (char)cp;
        } else if (cp < 0x800) {
            u[k++] = (char)(0xC0 | (cp >> 6));
            u[k++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            u[k++] = (char)(0xE0 | (cp >> 12));
            u[k++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            u[k++] = (char)(0x80 | (cp & 0x3F));
        }
        if (k) {
            append(c, u, k);
            return;
        }
    }
    append(c, s, n);
}

/* ---- Blocks -------------------------------------------------------------- */

static void list_item(ctx_t *c, const MD_BLOCK_LI_DETAIL *d)
{
    end_text_block(c);
    lv_obj_t *row = deck_box(top(c));
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);

    char mark[16];
    lv_color_t mc = g_pal.accent;
    list_t *l     = c->list_depth > 0 ? &c->lists[c->list_depth - 1] : NULL;
    if (d->is_task) {
        bool done = d->task_mark == 'x' || d->task_mark == 'X';
        snprintf(mark, sizeof(mark), "%s", done ? "[x]" : "[ ]");
        mc = done ? g_pal.ok : g_pal.dim;
    } else if (l && l->ordered) {
        snprintf(mark, sizeof(mark), "%u.", l->next++);
    } else {
        static const char *bullets[] = {">", "-", "\xC2\xB7"};
        snprintf(mark, sizeof(mark), "%s", bullets[(c->list_depth - 1) % 3]);
    }
    lv_obj_t *m = deck_label(row, g_font.mono_m, mc, mark);
    lv_obj_set_style_min_width(m, l && l->ordered ? 34 : 18, 0);

    lv_obj_t *body = column(row, 4);
    lv_obj_set_width(body, 0);
    lv_obj_set_flex_grow(body, 1);
    push(c, body);
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *ud)
{
    ctx_t *c = (ctx_t *)ud;
    if (c->depth == 1) c->blocks++;

    switch (type) {
        case MD_BLOCK_DOC: break;

        case MD_BLOCK_P: text_block(c, top(c), g_font.mono_m, g_pal.text); break;

        case MD_BLOCK_H: {
            unsigned level = ((MD_BLOCK_H_DETAIL *)detail)->level;
            const lv_font_t *f = level == 1 ? g_font.disp_l : level == 2 ? g_font.disp_m
                                 : level == 3 ? g_font.disp_s : g_font.mono_m;
            lv_color_t col = level <= 2 ? g_pal.accent : g_pal.accent2;
            text_block(c, top(c), f, col);
            lv_obj_set_style_margin_top(c->spans, c->blocks > 1 ? 10 : 0, 0);
            break;
        }

        case MD_BLOCK_QUOTE: {
            end_text_block(c);
            lv_obj_t *q = column(top(c), 8);
            lv_obj_set_style_bg_color(q, g_pal.panel_hi, 0);
            lv_obj_set_style_bg_opa(q, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(q, 4, 0);
            lv_obj_set_style_border_color(q, g_pal.accent2, 0);
            lv_obj_set_style_pad_all(q, 12, 0);
            lv_obj_set_style_pad_left(q, 18, 0);
            push(c, q);
            break;
        }

        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            end_text_block(c);
            if (c->list_depth < MAX_LISTS) {
                list_t *l   = &c->lists[c->list_depth];
                l->ordered  = type == MD_BLOCK_OL;
                l->next     = l->ordered ? ((MD_BLOCK_OL_DETAIL *)detail)->start : 0;
            }
            c->list_depth++;
            break;

        case MD_BLOCK_LI: list_item(c, (MD_BLOCK_LI_DETAIL *)detail); break;

        case MD_BLOCK_HR: {
            end_text_block(c);
            lv_obj_t *hr = lv_obj_create(top(c));
            lv_obj_remove_style_all(hr);
            lv_obj_set_size(hr, LV_PCT(100), 2);
            lv_obj_set_style_bg_color(hr, g_pal.line, 0);
            lv_obj_set_style_bg_opa(hr, LV_OPA_COVER, 0);
            lv_obj_set_style_margin_ver(hr, 8, 0);
            break;
        }

        case MD_BLOCK_CODE:
        case MD_BLOCK_HTML: {
            end_text_block(c);
            lv_obj_t *box = deck_box(top(c));
            lv_obj_set_width(box, LV_PCT(100));
            lv_obj_set_style_bg_color(box, lv_color_hex(0x04050A), 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(box, 1, 0);
            lv_obj_set_style_border_color(box, g_pal.line, 0);
            lv_obj_set_style_pad_all(box, 14, 0);
            c->code_label = deck_label(box, g_font.mono_m, type == MD_BLOCK_HTML ? g_pal.dim : g_pal.ok, "");
            lv_obj_set_width(c->code_label, LV_PCT(100));
            if (type == MD_BLOCK_CODE) {
                const MD_BLOCK_CODE_DETAIL *d = (MD_BLOCK_CODE_DETAIL *)detail;
                if (d->lang.size > 0) {
                    char lang[24];
                    snprintf(lang, sizeof(lang), "%.*s", (int)d->lang.size, d->lang.text);
                    lv_obj_t *tag = deck_label(box, g_font.mono_s, g_pal.dim, lang);
                    lv_obj_add_flag(tag, LV_OBJ_FLAG_IGNORE_LAYOUT);
                    lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, 0, -6);
                }
            }
            c->in_code_block = true;
            c->code_is_html  = type == MD_BLOCK_HTML;
            c->len           = 0;
            c->style_key     = 0;
            break;
        }

        case MD_BLOCK_TABLE: {
            end_text_block(c);
            c->table_cols = (int)((MD_BLOCK_TABLE_DETAIL *)detail)->col_count;
            if (c->table_cols < 1) c->table_cols = 1;
            lv_obj_t *t = column(top(c), 0);
            lv_obj_set_style_border_width(t, 1, 0);
            lv_obj_set_style_border_color(t, g_pal.line, 0);
            push(c, t);
            break;
        }
        case MD_BLOCK_THEAD: c->in_thead = true; break;
        case MD_BLOCK_TBODY: c->in_thead = false; break;
        case MD_BLOCK_TR: {
            lv_obj_t *r = deck_box(top(c));
            lv_obj_set_width(r, LV_PCT(100));
            lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
            if (c->in_thead) {
                lv_obj_set_style_bg_color(r, g_pal.panel_hi, 0);
                lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
            }
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, g_pal.line, 0);
            push(c, r);
            break;
        }
        case MD_BLOCK_TH:
        case MD_BLOCK_TD: {
            lv_obj_t *cell = deck_box(top(c));
            lv_obj_set_width(cell, LV_PCT(100 / c->table_cols));
            lv_obj_set_style_pad_all(cell, 8, 0);
            text_block(c, cell, g_font.mono_m, type == MD_BLOCK_TH ? g_pal.accent : g_pal.text);
            MD_ALIGN a = ((MD_BLOCK_TD_DETAIL *)detail)->align;
            if (a == MD_ALIGN_CENTER) lv_spangroup_set_align(c->spans, LV_TEXT_ALIGN_CENTER);
            if (a == MD_ALIGN_RIGHT) lv_spangroup_set_align(c->spans, LV_TEXT_ALIGN_RIGHT);
            push(c, cell);
            break;
        }
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *ud)
{
    (void)detail;
    ctx_t *c = (ctx_t *)ud;
    switch (type) {
        case MD_BLOCK_P:
            end_text_block(c);
            break;
        case MD_BLOCK_H: {
            lv_obj_t *parent = lv_obj_get_parent(c->spans);
            end_text_block(c);
            if (((MD_BLOCK_H_DETAIL *)detail)->level == 1) {
                lv_obj_t *rule = lv_obj_create(parent);
                lv_obj_remove_style_all(rule);
                lv_obj_set_size(rule, LV_PCT(100), 2);
                lv_obj_set_style_bg_color(rule, g_pal.accent, 0);
                lv_obj_set_style_bg_opa(rule, LV_OPA_40, 0);
            }
            break;
        }
        case MD_BLOCK_QUOTE:
            end_text_block(c);
            pop(c);
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
            if (c->list_depth > 0) c->list_depth--;
            break;
        case MD_BLOCK_LI:
            end_text_block(c);
            pop(c);
            break;
        case MD_BLOCK_CODE:
        case MD_BLOCK_HTML:
            if (c->len && c->buf[c->len - 1] == '\n') c->len--; /* drop the final newline */
            if (c->buf) {
                c->buf[c->len] = '\0';
                lv_label_set_text(c->code_label, c->buf);
            }
            c->len           = 0;
            c->in_code_block = false;
            c->code_label    = NULL;
            break;
        case MD_BLOCK_TABLE:
        case MD_BLOCK_TR:
            pop(c);
            break;
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            end_text_block(c);
            pop(c);
            break;
        default: break;
    }
    return 0;
}

/* ---- Spans and text ------------------------------------------------------ */

static int enter_span(MD_SPANTYPE type, void *detail, void *ud)
{
    (void)detail;
    ctx_t *c = (ctx_t *)ud;
    switch (type) {
        case MD_SPAN_STRONG: c->strong++; break;
        case MD_SPAN_EM: c->em++; break;
        case MD_SPAN_CODE: c->code++; break;
        case MD_SPAN_A: c->link++; break;
        case MD_SPAN_DEL: c->del++; break;
        case MD_SPAN_IMG: c->em++; append(c, "[img: ", 6); break;
        default: break;
    }
    return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *ud)
{
    (void)detail;
    ctx_t *c = (ctx_t *)ud;
    switch (type) {
        case MD_SPAN_STRONG: c->strong--; break;
        case MD_SPAN_EM: c->em--; break;
        case MD_SPAN_CODE: c->code--; break;
        case MD_SPAN_A: c->link--; break;
        case MD_SPAN_DEL: c->del--; break;
        case MD_SPAN_IMG: append(c, "]", 1); c->em--; break;
        default: break;
    }
    return 0;
}

static int text(MD_TEXTTYPE type, const MD_CHAR *s, MD_SIZE n, void *ud)
{
    ctx_t *c = (ctx_t *)ud;
    if (c->in_code_block) {
        append(c, s, n);
        return 0;
    }
    if (c->spans == NULL) {
        /* Tight list items deliver text without a paragraph block. */
        text_block(c, top(c), g_font.mono_m, g_pal.text);
    }
    switch (type) {
        case MD_TEXT_BR: append(c, "\n", 1); break;
        case MD_TEXT_SOFTBR: append(c, " ", 1); break;
        case MD_TEXT_ENTITY: append_entity(c, s, n); break;
        case MD_TEXT_NULLCHAR: break;
        default: append(c, s, n); break;
    }
    return 0;
}

/* ---- Entry --------------------------------------------------------------- */

int deck_md_render(lv_obj_t *parent, const char *src, size_t len)
{
    lv_obj_clean(parent);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 12, 0);

    ctx_t c = {0};
    c.stack[0] = parent;
    c.depth    = 1;
    c.font     = g_font.mono_m;
    c.color    = g_pal.text;

    MD_PARSER p = {0};
    p.abi_version = 0;
    p.flags       = MD_DIALECT_GITHUB | MD_FLAG_NOINDENTEDCODEBLOCKS;
    p.enter_block = enter_block;
    p.leave_block = leave_block;
    p.enter_span  = enter_span;
    p.leave_span  = leave_span;
    p.text        = text;

    md_parse(src, (MD_SIZE)len, &p, &c);
    end_text_block(&c);
    free(c.buf);
    return c.blocks;
}
