/*
 * SENTINEL > LOG: what SENTINEL has warned about today, newest first, read
 * back from the events file on storage. It opens on warnings only, because
 * that is the question; SHOW EVERYTHING adds the notes.
 *
 * Opening this screen is what marks warnings as seen: the status bar's
 * count clears here, not when SENTINEL is opened.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_notice.h"
#include "deck_theme.h"

#define TAIL_BYTES 8192 /* about a hundred rows */
#define ROWS_MAX   60
#define REFRESH_MS 2000
#define SCROLL_PX  150

static struct {
    lv_obj_t   *summary, *list;
    lv_timer_t *timer;
    bool        everything;
    uint32_t    shown_events; /* the count the list was built from */
    char        tail[TAIL_BYTES];
} s_log DECK_PSRAM_BSS;

static void toggle(void);

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

static lv_obj_t *cell(lv_obj_t *row, lv_color_t color, const char *text, int32_t width)
{
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, g_font.mono_b, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    if (width > 0) lv_obj_set_width(l, width);
    else lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(g_font.mono_b));
    return l;
}

static void rebuild(void)
{
    sentinel_store_stats_t st;
    sentinel_stats(&st);
    s_log.shown_events = st.events;
    lv_obj_clean(s_log.list);

    const char *root = hal_storage_root();
    int         len  = sentinel_store_tail(s_log.tail, sizeof(s_log.tail));
    int         rows = 0, hidden = 0;
    /* Walk the rows from the end of the buffer backwards: newest first. */
    for (char *end = s_log.tail + (len > 0 ? len : 0); end > s_log.tail && rows < ROWS_MAX;) {
        if (end[-1] == '\n') end[-1] = 0;
        char *start = end - 1;
        while (start > s_log.tail && start[-1] != '\n') start--;
        end = start;
        char *f[8];
        if (csv_split(start, f, 8) < 7 || strcmp(f[0], "time") == 0) continue; /* short row, or the header */
        sn_level_t lvl = sn_level_of(strcmp(f[1], "warn") == 0, f[3]);
        if (lvl == SN_NOTE && !s_log.everything) {
            hidden++;
            continue;
        }
        lv_obj_t *row = sn_row_box(s_log.list);
        lv_obj_set_style_pad_column(row, 14, 0);
        cell(row, g_pal.label, strlen(f[0]) >= 19 ? f[0] + 11 : f[0], 110);
        cell(row, sn_level_color(lvl), sn_level_mark(lvl), 30);
        cell(row, lvl == SN_NOTE ? g_pal.text : sn_level_color(lvl), f[3], 0);
        rows++;
    }
    if (!root) sn_set(s_log.summary, g_pal.warn, "! NO SD CARD AND NO INTERNAL STORAGE: NOTHING IS BEING KEPT.");
    else if (st.events_dropped) sn_set(s_log.summary, g_pal.warn, "! %u EVENTS COULD NOT BE WRITTEN TO STORAGE.", (unsigned)st.events_dropped);
    else if (s_log.everything) sn_set(s_log.summary, g_pal.label, "EVERYTHING FROM TODAY, NEWEST FIRST.   !! ALERT   ! WARNING   OTHERS ARE NOTES.");
    else sn_set(s_log.summary, g_pal.label, "TODAY'S WARNINGS, NEWEST FIRST.   !! ALERT   ! WARNING.   %d NOTES NOT SHOWN.", hidden);
    if (rows == 0) {
        lv_obj_t *l = lv_label_create(s_log.list);
        lv_obj_set_style_text_font(l, g_font.disp_l, 0);
        lv_obj_set_style_text_color(l, s_log.everything ? g_pal.label : g_pal.ok, 0);
        lv_label_set_text(l, s_log.everything ? "NOTHING LOGGED TODAY" : "NO WARNINGS TODAY");
    }
    sn_action_label(toggle, s_log.everything ? "WARNINGS ONLY" : "SHOW EVERYTHING");
}

static void toggle(void)
{
    s_log.everything = !s_log.everything;
    rebuild();
}

static void scroll(int dir)
{
    int32_t dy = dir < 0 ? SCROLL_PX : -SCROLL_PX; /* scroll_by moves the content, so up is positive */
    if (dir < 0 && lv_obj_get_scroll_y(s_log.list) < SCROLL_PX) dy = lv_obj_get_scroll_y(s_log.list);
    if (dir > 0 && lv_obj_get_scroll_bottom(s_log.list) < SCROLL_PX) dy = -lv_obj_get_scroll_bottom(s_log.list);
    if (dy) lv_obj_scroll_by(s_log.list, 0, dy, LV_ANIM_OFF);
}
static void scroll_up(void) { scroll(-1); }
static void scroll_down(void) { scroll(1); }

/* New rows arrive while the screen is open: reload, but only while the list
 * is at the top, so reading older rows is not yanked away. */
static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_store_stats_t st;
    sentinel_stats(&st);
    deck_notice_seen(); /* they are being read */
    if (st.events != s_log.shown_events && lv_obj_get_scroll_y(s_log.list) <= 0) rebuild();
}

static void build(lv_obj_t *body)
{
    bool keep = s_log.everything;
    memset(&s_log, 0, sizeof(s_log));
    s_log.everything = keep;
    deck_notice_seen();
    s_log.summary = sn_note(body);
    s_log.list    = lv_obj_create(body);
    lv_obj_remove_style_all(s_log.list);
    lv_obj_set_width(s_log.list, LV_PCT(100));
    lv_obj_set_flex_grow(s_log.list, 1);
    lv_obj_set_flex_flow(s_log.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_log.list, 10, 0);
    lv_obj_add_flag(s_log.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_log.list, LV_DIR_VER);
    sn_action(HID_UP, "UP", "NEWER", scroll_up);
    sn_action(HID_DOWN, "DOWN", "OLDER", scroll_down);
    sn_action(HID_LETTER('E'), "E", "SHOW EVERYTHING", toggle);
    sn_help("Everything SENTINEL notices is written to a file on storage, one a day, as plain CSV with the time and, when "
            "there is a GPS fix, the place. This is today's.\n\n"
            "!! is an alert: something happening now, like likely jamming or a deauthentication flood.\n"
            "!  is a warning: something to look at, like a tracker that has stayed with you or an open copy of a known "
            "network.\n"
            "Everything else is a note.\n\n"
            "Read the files in FILES or over the portal: they are in the sentinel folder.");
    rebuild();
    s_log.timer = lv_timer_create(tick, REFRESH_MS, NULL);
}

static void destroy(void)
{
    if (s_log.timer) lv_timer_delete(s_log.timer);
    s_log.timer = NULL;
    s_log.list = s_log.summary = NULL;
}

static void status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)st;
    unsigned unseen = deck_notice_unseen();
    if (unseen) {
        snprintf(line, n, "%u WARNING%s NOT YET SEEN", unseen, unseen == 1 ? "" : "S");
        *level = SN_WARNING;
    } else {
        snprintf(line, n, "NOTHING NEW");
    }
}

const sn_screen_t sn_log_screen = {
    .name    = "LOG",
    .purpose = "What SENTINEL has warned about today.",
    .icon    = ICON_HISTORY,
    .build   = build,
    .destroy = destroy,
    .status  = status,
};
