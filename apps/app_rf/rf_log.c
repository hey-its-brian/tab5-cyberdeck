/*
 * RF WATCH > LOG: today's events file from storage, newest first. The rows
 * are what sentinel_store wrote (time, level, source, text, position), so
 * this is the history the eight-entry ring on OVERVIEW cannot hold: a chirp
 * from an hour ago is still here, with where the deck was when it happened.
 */
#include "rf_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TAIL_BYTES 8192 /* about a hundred rows */
#define ROWS_MAX   80
#define REFRESH_MS 2000
#define SCROLL_PX  120

static struct {
    lv_obj_t   *summary, *list;
    lv_timer_t *timer;
    uint32_t    shown_events; /* the count the list was built from */
    char        tail[TAIL_BYTES];
} s_log DECK_PSRAM_BSS;

/* Split one CSV row in place (RFC 4180 quoting). Returns the field count. */
static int csv_split(char *row, char *f[], int max)
{
    int   n = 0;
    char *p = row;
    while (n < max) {
        char *out = p;
        f[n++]    = out;
        if (*p == '"') {
            p++;
            while (*p && !(*p == '"' && p[1] != '"')) {
                if (*p == '"') p++; /* a doubled quote is one quote */
                *out++ = *p++;
            }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ',') *out++ = *p++;
        }
        bool more = *p == ',';
        if (*p) p++;
        *out = 0;
        if (!more) break;
    }
    return n;
}

static void rebuild(void)
{
    sentinel_store_stats_t st;
    sentinel_stats(&st);
    s_log.shown_events = st.events;
    lv_obj_clean(s_log.list);

    const char *root = hal_storage_root();
    int         len  = sentinel_store_tail(s_log.tail, sizeof(s_log.tail));
    lv_label_set_text_fmt(s_log.summary, "%s/sentinel   ::   THIS SESSION: %u EVENTS WRITTEN, %u LOST, %u FRAMES CAPTURED",
                          root ? root : "NO STORAGE", (unsigned)st.events, (unsigned)st.events_dropped,
                          (unsigned)st.packets);
    if (len <= 0) {
        deck_label(s_log.list, g_font.mono_m, g_pal.dim, root ? "NOTHING LOGGED TODAY YET" : "NO SD CARD AND NO INTERNAL STORAGE");
        return;
    }

    /* Walk the rows from the end of the buffer backwards: newest first. */
    int rows = 0;
    for (char *end = s_log.tail + len; end > s_log.tail && rows < ROWS_MAX;) {
        if (end[-1] == '\n') end[-1] = 0;
        char *start = end - 1;
        while (start > s_log.tail && start[-1] != '\n') start--;
        end = start;
        char *f[8];
        if (csv_split(start, f, 8) < 7 || strcmp(f[0], "time") == 0) continue; /* short row, or the header */
        bool        warn = strcmp(f[1], "warn") == 0;
        const char *hms  = strlen(f[0]) >= 19 ? f[0] + 11 : f[0];
        char        pos[40];
        if (f[4][0] && f[5][0]) snprintf(pos, sizeof(pos), "%.9s, %.10s", f[4], f[5]);
        else snprintf(pos, sizeof(pos), "NO FIX");

        lv_obj_t *row = deck_box(s_log.list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 16, 0);
        lv_color_t c = warn ? g_pal.warn : g_pal.text;
        deck_label(row, g_font.mono_m, g_pal.dim, hms);
        lv_obj_t *src = deck_label(row, g_font.mono_m, g_pal.accent2, f[2]);
        lv_obj_set_width(src, 64);
        lv_obj_t *text = deck_label(row, g_font.mono_m, c, f[3]);
        lv_obj_set_flex_grow(text, 1);
        lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
        deck_label(row, g_font.mono_s, g_pal.dim, pos);
        rows++;
    }
    if (rows == 0) deck_label(s_log.list, g_font.mono_m, g_pal.dim, "NOTHING LOGGED TODAY YET");
}

/* New rows arrive while the screen is open: reload, but only while the list
 * is at the top, so reading older rows is not yanked away. */
static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_store_stats_t st;
    sentinel_stats(&st);
    if (st.events != s_log.shown_events && lv_obj_get_scroll_y(s_log.list) <= 0) rebuild();
}

void rf_log_build(lv_obj_t *parent)
{
    memset(&s_log, 0, sizeof(s_log));
    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 10, 0);

    lv_obj_t *p = rf_panel(root, "EVENT LOG  TODAY  NEWEST FIRST");
    lv_obj_set_flex_grow(p, 1);
    s_log.summary = deck_label(p, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_width(s_log.summary, LV_PCT(100));
    lv_label_set_long_mode(s_log.summary, LV_LABEL_LONG_MODE_DOTS);

    s_log.list = deck_box(p);
    lv_obj_set_width(s_log.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_log.list, 1);
    lv_obj_set_flex_flow(s_log.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_log.list, 6, 0);
    lv_obj_add_flag(s_log.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_log.list, LV_DIR_VER);

    deck_label(root, g_font.mono_s, g_pal.dim,
               "[UP/DOWN] SCROLL   [R] RELOAD   [ESC] TOOLS      THE FILES ARE PLAIN CSV: READ THEM IN FILES OR OVER THE PORTAL");
    rebuild();
    s_log.timer = lv_timer_create(tick, REFRESH_MS, NULL);
}

void rf_log_destroy(void)
{
    if (s_log.timer) lv_timer_delete(s_log.timer);
    s_log.timer = NULL;
    s_log.list = s_log.summary = NULL;
}

bool rf_log_key(const deck_key_t *k)
{
    if (!s_log.list || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) return false;
    if (k->code == HID_UP || k->code == HID_DOWN) {
        int32_t dy = k->code == HID_UP ? SCROLL_PX : -SCROLL_PX; /* scroll_by moves the content, so up is positive */
        if (k->code == HID_UP && lv_obj_get_scroll_y(s_log.list) < SCROLL_PX) dy = lv_obj_get_scroll_y(s_log.list);
        if (k->code == HID_DOWN && lv_obj_get_scroll_bottom(s_log.list) < SCROLL_PX)
            dy = -lv_obj_get_scroll_bottom(s_log.list);
        if (dy) lv_obj_scroll_by(s_log.list, 0, dy, LV_ANIM_OFF);
        return true;
    }
    if (k->code == HID_LETTER('R')) {
        rebuild();
        return true;
    }
    return false;
}
