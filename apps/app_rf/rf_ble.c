/*
 * RF WATCH > BLUETOOTH: who is advertising nearby. The C5 listens (a
 * passive scan: it never sends anything), labels what it can recognise and
 * reports devices; this screen lists them, strongest first.
 *
 * What the labels mean, and their limits, are in sentinel_ble.h. Trackers
 * (an AirTag or other Find My tag away from its owner, SmartTag, Tile,
 * Chipolo, a Google tracker) are yellow; surveillance devices (Flock, Axon)
 * are red. The warning for a tracker that stays with you comes from the
 * track watch in sentinel_blehost.h, and reaches you in any app.
 *
 * While this screen is open the C5 gives Bluetooth its 2.4 GHz radio full
 * time (its Wi-Fi scan waits); leaving hands the radio back and ends any
 * capture.
 */
#include "rf_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_theme.h"
#include "deck_widgets.h"
#include "sentinel_cmd.h"

#define TICK_MS 500
#define ROWS    15

typedef enum { FILTER_ALL, FILTER_TRACKERS, FILTER_SURVEILLANCE, FILTER_N } ble_filter_t;
static const char *const s_filter_name[FILTER_N] = {"ALL", "TRACKERS", "SURVEILLANCE"};

static struct {
    lv_obj_t *count, *rate, *threats, *radio, *capture, *filter_line;
    lv_obj_t *sel_addr, *sel_kind, *sel_who, *sel_sig, *sel_track;
    lv_obj_t *row[ROWS], *more;
    lv_timer_t *timer;
    ble_filter_t filter;
    bool     have_sel;      /* the selection is a device, followed by address as rows reorder */
    uint8_t  sel[6];
    int      sel_index;     /* its place in the filtered list, kept when the device goes */
    bool     cap_wanted;
    uint32_t cap_base;      /* frames stored before this capture began */
    uint32_t lost_base;     /* and frames lost before it began */
    int      shown[ROWS];   /* table index drawn on each row, -1 for none */
    int      match_n;
    sentinel_state_t  st;
    sentinel_bletab_t tab;
} s_ble DECK_PSRAM_BSS;

static bool passes(const sentinel_bdv_t *d)
{
    switch (s_ble.filter) {
        case FILTER_TRACKERS: return sentinel_ble_is_tracker(d->kind);
        case FILTER_SURVEILLANCE: return sentinel_ble_is_surveillance(d->kind);
        default: return true;
    }
}

static lv_color_t kind_color(const sentinel_bdv_t *d, bool mine)
{
    if (mine) return g_pal.ok;
    if (sentinel_ble_is_surveillance(d->kind)) return g_pal.danger;
    if (sentinel_ble_is_tracker(d->kind)) return g_pal.warn;
    return d->kind == SENTINEL_BLE_UNKNOWN ? g_pal.text : g_pal.accent; /* recognised and harmless */
}

static void upper(char *s)
{
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

static void fmt_span(char *out, size_t n, uint32_t ms)
{
    unsigned s = (unsigned)(ms / 1000);
    if (s < 60) snprintf(out, n, "%u S", s);
    else if (s < 3600) snprintf(out, n, "%u MIN", s / 60);
    else snprintf(out, n, "%u H %u MIN", s / 3600, s / 60 % 60);
}

static void render_summary(void)
{
    const sentinel_state_t *st = &s_ble.st;
    int trackers = 0, cameras = 0;
    for (int i = 0; i < s_ble.tab.n; i++) {
        if (sentinel_ble_is_tracker(s_ble.tab.dev[i].d.kind)) trackers++;
        if (sentinel_ble_is_surveillance(s_ble.tab.dev[i].d.kind)) cameras++;
    }
    lv_label_set_text_fmt(s_ble.count, "%d", s_ble.tab.n);
    lv_label_set_text_fmt(s_ble.rate, "%u ADVERTS/S    STRONGEST %d dBm", st->ble_rate,
                          s_ble.tab.n ? s_ble.tab.dev[0].d.rssi : -127);
    lv_label_set_text_fmt(s_ble.threats, "TRACKERS %d    SURVEILLANCE %d", trackers, cameras);
    lv_obj_set_style_text_color(s_ble.threats, cameras ? g_pal.danger : trackers ? g_pal.warn : g_pal.dim, 0);

    bool link = sentinel_link(st);
    const char *radio = !link ? "WAITING FOR THE C5"
                        : !st->ble_ok ? "THIS NODE HAS NO BLUETOOTH (FIRMWARE BEFORE 0.4.0)"
                        : !strcmp(st->mode, "ble") ? "RADIO: BLUETOOTH FULL TIME, WI-FI SCAN WAITING"
                                                    : "RADIO: SHARED WITH WI-FI, ASKING FOR FULL TIME";
    lv_label_set_text(s_ble.radio, radio);

    sentinel_store_stats_t log;
    sentinel_stats(&log);
    if (st->capture) {
        unsigned lost = (unsigned)(log.packets_dropped - s_ble.lost_base);
        if (lost) lv_label_set_text_fmt(s_ble.capture, "CAPTURE ON  ::  %u FRAMES TO SD, %u LOST", (unsigned)(log.packets - s_ble.cap_base), lost);
        else lv_label_set_text_fmt(s_ble.capture, "CAPTURE ON  ::  %u FRAMES TO SD", (unsigned)(log.packets - s_ble.cap_base));
        lv_obj_set_style_text_color(s_ble.capture, lost ? g_pal.warn : g_pal.accent, 0);
    } else {
        /* Captured frames need the fast link; the core does not even ask on a slow one. */
        if (s_ble.cap_wanted && st->host_baud < SENTINEL_CAP_MIN_BAUD)
            lv_label_set_text_fmt(s_ble.capture, "CAPTURE NEEDS THE FAST LINK (NOW %u BAUD)", (unsigned)st->host_baud);
        else lv_label_set_text(s_ble.capture, s_ble.cap_wanted ? "CAPTURE: ASKING THE C5" : "CAPTURE OFF");
        lv_obj_set_style_text_color(s_ble.capture, s_ble.cap_wanted ? g_pal.warn : g_pal.dim, 0);
        s_ble.cap_base  = log.packets;
        s_ble.lost_base = log.packets_dropped;
    }
    lv_label_set_text_fmt(s_ble.filter_line, "SHOWING %s  ::  %d OF %d", s_filter_name[s_ble.filter], s_ble.match_n, s_ble.tab.n);
}

static void render_selected(const sentinel_bletab_dev_t *e)
{
    if (!e) {
        lv_label_set_text(s_ble.sel_addr, "--:--:--:--:--:--");
        lv_label_set_text(s_ble.sel_kind, s_ble.match_n ? "" : "NOTHING TO SHOW");
        lv_label_set_text(s_ble.sel_who, "");
        lv_label_set_text(s_ble.sel_sig, "");
        lv_label_set_text(s_ble.sel_track, "");
        return;
    }
    const sentinel_bdv_t *d = &e->d;
    bool mine = sentinel_ble_mine(d->addr);
    char addr[18], kind[32], span[24];
    sentinel_ble_addr_text(addr, d->addr);
    lv_label_set_text_fmt(s_ble.sel_addr, "%s  %s", addr, d->pub ? "PUBLIC" : "RANDOM");
    snprintf(kind, sizeof(kind), "%s", d->kind == SENTINEL_BLE_UNKNOWN ? "NOT RECOGNISED" : sentinel_ble_kind_label(d->kind));
    upper(kind);
    lv_label_set_text_fmt(s_ble.sel_kind, "%s%s", kind, mine ? "  ::  MINE" : "");
    lv_obj_set_style_text_color(s_ble.sel_kind, kind_color(d, mine), 0);
    const char *vendor = sentinel_ble_company(d->company);
    if (vendor) lv_label_set_text_fmt(s_ble.sel_who, "%s   %s", vendor, d->name);
    else if (d->company) lv_label_set_text_fmt(s_ble.sel_who, "VENDOR 0x%04X   %s", d->company, d->name);
    else lv_label_set_text_fmt(s_ble.sel_who, "%s", d->name[0] ? d->name : "NO NAME, NO VENDOR");
    fmt_span(span, sizeof(span), sentinel_now_ms() - e->first_ms);
    lv_label_set_text_fmt(s_ble.sel_sig, "%d dBm   %u ADVERTS   LISTED %s", d->rssi, d->count, span);

    sentinel_track_t t;
    if (mine) {
        lv_label_set_text(s_ble.sel_track, "MARKED AS YOURS: NO ALERTS. [M] UNMARKS IT.");
    } else if (sentinel_ble_track(d->addr, &t)) {
        fmt_span(span, sizeof(span), sentinel_now_ms() - t.first_ms);
        if (sentinel_ble_is_surveillance(d->kind)) lv_label_set_text_fmt(s_ble.sel_track, "IN EARSHOT FOR %s", span);
        else if (t.has_pos) lv_label_set_text_fmt(s_ble.sel_track, "WITH YOU %s, YOU MOVED %u M%s", span, (unsigned)t.moved_m, t.alerted ? "  ::  WARNED" : "");
        else lv_label_set_text_fmt(s_ble.sel_track, "WITH YOU %s, NO GPS FIX%s", span, t.alerted ? "  ::  WARNED" : "");
    } else if (d->kind == SENTINEL_BLE_FINDMY) {
        lv_label_set_text(s_ble.sel_track, "ITS OWNER IS NEAR IT: NOT A TRACKING RISK");
    } else {
        lv_label_set_text(s_ble.sel_track, "");
    }
}

static void render(void)
{
    /* The filtered list, and where the selection sits in it. */
    static int match[SENTINEL_BLETAB_MAX];
    int n = 0, sel = -1;
    for (int i = 0; i < s_ble.tab.n; i++) {
        if (!passes(&s_ble.tab.dev[i].d)) continue;
        if (s_ble.have_sel && memcmp(s_ble.tab.dev[i].d.addr, s_ble.sel, 6) == 0) sel = n;
        match[n++] = i;
    }
    s_ble.match_n = n;
    if (sel < 0 && n) { /* the device went (or nothing was chosen yet): stay at the same place in the list */
        sel = s_ble.sel_index < n ? s_ble.sel_index : n - 1;
        memcpy(s_ble.sel, s_ble.tab.dev[match[sel]].d.addr, 6);
        s_ble.have_sel = true;
    }
    if (n == 0) s_ble.have_sel = false;
    s_ble.sel_index = sel < 0 ? 0 : sel;

    /* A window of ROWS around the selection. */
    int top = sel - ROWS / 2;
    if (top > n - ROWS) top = n - ROWS;
    if (top < 0) top = 0;
    for (int r = 0; r < ROWS; r++) {
        int k = top + r;
        lv_obj_t *row = s_ble.row[r];
        if (k >= n) {
            lv_label_set_text(row, r == 0 && n == 0 ? (s_ble.tab.n ? "NOTHING MATCHES THIS FILTER" : "LISTENING...") : "");
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(row, g_pal.dim, 0);
            s_ble.shown[r] = -1;
            continue;
        }
        const sentinel_bdv_t *d = &s_ble.tab.dev[match[k]].d;
        bool mine = sentinel_ble_mine(d->addr);
        char addr[18], kind[12];
        sentinel_ble_addr_text(addr, d->addr);
        snprintf(kind, sizeof(kind), "%s", d->kind == SENTINEL_BLE_UNKNOWN ? "-" : sentinel_ble_kind_word(d->kind));
        upper(kind);
        const char *vendor = sentinel_ble_company(d->company);
        lv_label_set_text_fmt(row, "%4d  %-9s %s  %-10s %s%s", d->rssi, kind, addr, vendor ? vendor : "", d->name, mine ? "  (MINE)" : "");
        lv_obj_set_style_text_color(row, kind_color(d, mine), 0);
        lv_obj_set_style_bg_opa(row, k == sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        s_ble.shown[r] = match[k];
    }
    if (n > ROWS) lv_label_set_text_fmt(s_ble.more, "%d TO %d OF %d", top + 1, top + ROWS, n);
    else lv_label_set_text(s_ble.more, "");

    render_summary();
    render_selected(sel >= 0 ? &s_ble.tab.dev[match[sel]] : NULL);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_ble.st);
    sentinel_ble_table(&s_ble.tab);
    render();
}

static void row_clicked(lv_event_t *e)
{
    int r = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_ble.shown[r] < 0) return;
    memcpy(s_ble.sel, s_ble.tab.dev[s_ble.shown[r]].d.addr, 6);
    s_ble.have_sel = true;
    render();
}

void rf_ble_build(lv_obj_t *parent)
{
    memset(&s_ble, 0, sizeof(s_ble));
    sentinel_tool("ble"); /* Bluetooth gets the radio full time while this screen is up */

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 10, 0);

    lv_obj_t *cols = deck_box(root);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_flex_grow(cols, 1);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 16, 0);

    lv_obj_t *left = deck_box(cols);
    lv_obj_set_size(left, 440, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);

    lv_obj_t *p   = rf_panel(left, "BLUETOOTH LE  PASSIVE SCAN");
    lv_obj_t *big = deck_box(p);
    lv_obj_set_width(big, LV_PCT(100));
    lv_obj_set_flex_flow(big, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(big, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(big, 14, 0);
    s_ble.count = deck_label(big, g_font.disp_l, g_pal.text, "0");
    deck_label(big, g_font.mono_m, g_pal.dim, "DEVICES IN EARSHOT");
    s_ble.rate        = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_ble.threats     = deck_label(p, g_font.mono_m, g_pal.dim, "");
    s_ble.radio       = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_ble.capture     = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_ble.filter_line = deck_label(p, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_width(s_ble.radio, LV_PCT(100));
    lv_label_set_long_mode(s_ble.radio, LV_LABEL_LONG_MODE_WRAP);

    p = rf_panel(left, "SELECTED");
    lv_obj_set_flex_grow(p, 1);
    s_ble.sel_addr  = deck_label(p, g_font.mono_m, g_pal.text, "");
    s_ble.sel_kind  = deck_label(p, g_font.disp_s, g_pal.text, "");
    s_ble.sel_who   = deck_label(p, g_font.mono_s, g_pal.text, "");
    s_ble.sel_sig   = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_ble.sel_track = deck_label(p, g_font.mono_s, g_pal.dim, "");
    lv_obj_t *wrap[] = {s_ble.sel_who, s_ble.sel_track, s_ble.sel_kind};
    for (size_t i = 0; i < sizeof(wrap) / sizeof(wrap[0]); i++) {
        lv_obj_set_width(wrap[i], LV_PCT(100));
        lv_label_set_long_mode(wrap[i], LV_LABEL_LONG_MODE_WRAP);
    }

    lv_obj_t *right = deck_box(cols);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    p = rf_panel(right, "DEVICES  STRONGEST FIRST   dBm  WHAT  ADDRESS  VENDOR  NAME");
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_style_pad_row(p, 2, 0);
    for (int r = 0; r < ROWS; r++) {
        lv_obj_t *row = deck_label(p, g_font.mono_m, g_pal.dim, "");
        lv_obj_set_width(row, LV_PCT(100));
        lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_bg_color(row, g_pal.panel_hi, 0);
        lv_obj_set_style_pad_ver(row, 4, 0);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_height(row, lv_font_get_line_height(g_font.mono_m) + 8); /* one line: a long name ends in dots, it does not wrap */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)r);
        s_ble.row[r]   = row;
        s_ble.shown[r] = -1;
    }
    s_ble.more = deck_label(p, g_font.mono_s, g_pal.dim, "");

    deck_label(root, g_font.mono_s, g_pal.dim,
               "[UP/DOWN] SELECT   [F] FILTER   [M] MARK AS MINE   [C] CAPTURE TO SD   [ESC] TOOLS      A LABEL IS A MATCH, NOT PROOF");

    s_ble.timer = lv_timer_create(tick, TICK_MS, NULL);
    tick(s_ble.timer);
}

void rf_ble_destroy(void)
{
    if (s_ble.timer) lv_timer_delete(s_ble.timer);
    s_ble.timer = NULL;
    sentinel_capture(false); /* capture belongs to this screen */
    sentinel_tool("watch");  /* and so does the radio: back to sharing it */
    memset(s_ble.row, 0, sizeof(s_ble.row));
}

bool rf_ble_key(const deck_key_t *k)
{
    if (!s_ble.timer || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) return false;
    if (k->code == HID_UP || k->code == HID_DOWN) {
        int n = s_ble.match_n, i = s_ble.sel_index + (k->code == HID_DOWN ? 1 : -1);
        if (n == 0) return true;
        i = i < 0 ? 0 : i >= n ? n - 1 : i;
        /* move the selection to the i-th device that passes the filter */
        for (int t = 0, m = 0; t < s_ble.tab.n; t++) {
            if (!passes(&s_ble.tab.dev[t].d)) continue;
            if (m++ == i) {
                memcpy(s_ble.sel, s_ble.tab.dev[t].d.addr, 6);
                s_ble.have_sel = true;
                break;
            }
        }
        render();
        return true;
    }
    if (k->code == HID_LETTER('F')) {
        s_ble.filter    = (ble_filter_t)((s_ble.filter + 1) % FILTER_N);
        s_ble.have_sel  = false; /* a new list: start at its top */
        s_ble.sel_index = 0;
        render();
        return true;
    }
    if (k->code == HID_LETTER('M')) {
        if (s_ble.have_sel) sentinel_ble_set_mine(s_ble.sel, !sentinel_ble_mine(s_ble.sel));
        render();
        return true;
    }
    if (k->code == HID_LETTER('C')) {
        s_ble.cap_wanted = !s_ble.cap_wanted;
        sentinel_capture(s_ble.cap_wanted);
        render();
        return true;
    }
    return false;
}
