/*
 * RF WATCH > OVERVIEW: the RF Sentinel board at a glance. Link and board
 * health with the session's mode and arm state, GNSS, the Wi-Fi the C5
 * hears, the 433 MHz sweep, 2.4 GHz occupancy and the newest events.
 * deck_sentinel powers the board and parses its stream; this file only
 * draws the latest snapshot four times a second.
 */
#include "rf_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TICK_MS  250
#define EVT_ROWS 5
#define CHART_H  88

enum { CHIP_CC, CHIP_NRF, CHIP_GPS, CHIP_WIFI, CHIP_N };
static const char *const s_chip_name[CHIP_N] = {"CC1101", "NRF24", "GPS", "WI-FI"};

static struct {
    lv_obj_t *link_word, *link_line, *chip[CHIP_N], *lines, *pins, *session, *hint;
    lv_obj_t *gps_sats, *gps_fix, *gps_q, *gps_pos, *gps_alt;
    lv_obj_t *wifi_sum, *ap_row[SENTINEL_AP_MAX];
    lv_obj_t *sub_now, *sub_peak, *sub_status;
    lv_chart_series_t *sub_now_s, *sub_peak_s;
    lv_obj_t *nrf_now, *nrf_peak, *nrf_status;
    lv_chart_series_t *nrf_now_s, *nrf_peak_s;
    lv_obj_t *evt_row[EVT_ROWS];
    lv_timer_t *timer;
    sentinel_state_t st;
} s_ui DECK_PSRAM_BSS;

/* ---- Helpers ------------------------------------------------------------- */

static void chip_set(lv_obj_t *chip, const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_obj_get_child(chip, 0);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    deck_panel_set_outline(chip, color);
}

static void fmt_uptime(char *out, size_t n, uint32_t s)
{
    snprintf(out, n, "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

static const char *fix_word(uint8_t fix)
{
    return fix >= 3 ? "3D FIX" : fix == 2 ? "2D FIX" : fix == 1 ? "FIX" : "NO FIX";
}

/* "hhmmss.ss" from the GPS as hh:mm:ss. */
static void fmt_utc(char *out, size_t n, const char *utc)
{
    if (strlen(utc) < 6) {
        snprintf(out, n, "--:--:--");
        return;
    }
    snprintf(out, n, "%.2s:%.2s:%.2s", utc, utc + 2, utc + 4);
}

/* A bar chart with one series. Two of them stacked in `box` make a peak-hold
 * display: the faint one behind holds the peaks, the bright one in front
 * shows the latest sweep. */
static lv_obj_t *bars(lv_obj_t *box, int points, int max, lv_color_t color, lv_opa_t opa, lv_chart_series_t **series)
{
    lv_obj_t *c = lv_chart_create(box);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(c, 0, 0);
    lv_chart_set_type(c, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(c, (uint32_t)points);
    lv_chart_set_div_line_count(c, 3, 0);
    lv_chart_set_axis_range(c, LV_CHART_AXIS_PRIMARY_Y, 0, max);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_column(c, points > 30 ? 1 : 4, 0);
    lv_obj_set_style_line_color(c, g_pal.grid, LV_PART_MAIN);
    lv_obj_set_style_line_opa(c, opa == LV_OPA_COVER ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(c, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(c, opa, LV_PART_ITEMS);
    *series = lv_chart_add_series(c, color, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(c, *series, 0);
    return c;
}

/* The box the two charts stack in, plus a row of tick labels under it. */
static lv_obj_t *chart_box(lv_obj_t *parent, const char *const *ticks, int tick_n)
{
    lv_obj_t *box = deck_box(parent);
    lv_obj_set_size(box, LV_PCT(100), CHART_H);
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < tick_n; i++) deck_label(row, g_font.mono_s, g_pal.dim, ticks[i]);
    return box;
}

/* ---- Rendering ----------------------------------------------------------- */

static void render_link(void)
{
    const sentinel_state_t *st = &s_ui.st;
    bool link = sentinel_link(st);
    char up[16], line[160];
    fmt_uptime(up, sizeof(up), st->uptime_s);
    if (!st->powered) {
        lv_label_set_text(s_ui.link_word, "BOARD OFF");
        lv_obj_set_style_text_color(s_ui.link_word, g_pal.danger, 0);
        snprintf(line, sizeof(line), "EXT5V AND THE UART COULD NOT BE STARTED. SEE THE LOG.");
    } else if (!link) {
        lv_label_set_text(s_ui.link_word, st->frames ? "LINK LOST" : "NO LINK");
        lv_obj_set_style_text_color(s_ui.link_word, g_pal.warn, 0);
        snprintf(line, sizeof(line), "EXT5V ON  RX_EN HIGH  ::  WAITING FOR THE C5");
    } else {
        lv_label_set_text(s_ui.link_word, "ONLINE");
        lv_obj_set_style_text_color(s_ui.link_word, g_pal.ok, 0);
        snprintf(line, sizeof(line), "FW %s  PROTO %s  ::  UP %s  ::  HEAP %uK", st->fw, st->proto, up,
                 (unsigned)(st->heap / 1024));
    }
    lv_label_set_text(s_ui.link_line, line);

    bool ok[CHIP_N] = {st->cc_ok, st->nrf_ok, st->gps_ok, st->wifi_ok};
    for (int i = 0; i < CHIP_N; i++) {
        lv_color_t c = !link ? g_pal.dim : ok[i] ? g_pal.ok : g_pal.danger;
        chip_set(s_ui.chip[i], s_chip_name[i], c);
    }
    char tool[sizeof(st->mode)];
    snprintf(tool, sizeof(tool), "%s", st->mode);
    for (char *c = tool; *c; c++)
        if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    lv_label_set_text_fmt(s_ui.lines, "FRAMES %u  BAD %u  ::  %u BAUD", (unsigned)st->frames, (unsigned)st->bad,
                          (unsigned)st->host_baud);
    lv_label_set_text_fmt(s_ui.pins, "TOOL %s  ::  GDO0 %d  GDO2 %d  NRF IRQ %s", tool, st->gdo0, st->gdo2,
                          st->nrf_irq ? "IDLE" : "LOW");

    /* The session: what was chosen at the gate, and whether the node agrees. */
    if (rf_mode() == RF_MODE_SAFE) {
        lv_label_set_text(s_ui.session, "SESSION SAFE  ::  TRANSMIT TESTS LOCKED");
        lv_obj_set_style_text_color(s_ui.session, g_pal.ok, 0);
    } else if (sentinel_armed(st)) {
        lv_label_set_text_fmt(s_ui.session, "SESSION RED TEAM  ::  NODE ARMED, LEASE %.1f S",
                              (double)sentinel_arm_left_ms(&st->arm, sentinel_now_ms()) / 1000.0);
        lv_obj_set_style_text_color(s_ui.session, g_pal.danger, 0);
    } else {
        lv_label_set_text(s_ui.session, "SESSION RED TEAM  ::  NODE NOT ARMED YET");
        lv_obj_set_style_text_color(s_ui.session, g_pal.warn, 0);
    }
}

static void render_hint(void)
{
    sentinel_store_stats_t log;
    sentinel_stats(&log);
    lv_label_set_text_fmt(s_ui.hint, "[H] HELLO   [P] PING   [T] TEST THE TX GATE   [ESC] TOOLS      LOG %u EVENTS  %u LOST",
                          (unsigned)log.events, (unsigned)log.events_dropped);
}

static void render_gps(void)
{
    const sentinel_state_t *st = &s_ui.st;
    bool have = sentinel_link(st) && st->gps_ok;
    lv_label_set_text_fmt(s_ui.gps_sats, "%d", have ? st->sats_used : 0);
    lv_obj_set_style_text_color(s_ui.gps_sats, have && st->fix ? g_pal.text : g_pal.dim, 0);
    chip_set(s_ui.gps_fix, have ? fix_word(st->fix) : "NO GPS", have && st->fix ? g_pal.ok : g_pal.warn);
    char utc[12];
    fmt_utc(utc, sizeof(utc), st->utc);
    lv_label_set_text_fmt(s_ui.gps_q, "IN VIEW %d    HDOP %.1f    C/N0 %.0f AVG  %d MAX", st->sats_view, (double)st->hdop,
                          (double)st->cn0_avg, st->cn0_max);
    if (have && st->fix) {
        lv_label_set_text_fmt(s_ui.gps_pos, "LAT %11.6f    LON %11.6f", st->lat, st->lon);
        lv_label_set_text_fmt(s_ui.gps_alt, "ALT %.0f M    UTC %s", (double)st->alt_m, utc);
    } else {
        lv_label_set_text(s_ui.gps_pos, "LAT  ---.------    LON  ---.------");
        lv_label_set_text_fmt(s_ui.gps_alt, "ALT --- M    UTC %s", utc);
    }
}

static void render_wifi(void)
{
    const sentinel_state_t *st = &s_ui.st;
    if (!st->wifi_scans) {
        lv_label_set_text(s_ui.wifi_sum, "NO SCAN FROM THE C5 YET");
    } else {
        lv_label_set_text_fmt(s_ui.wifi_sum, "%u NETWORKS   %u @ 2.4G   %u @ 5G   BEST %d dBm", st->wifi_total,
                              st->wifi_24, st->wifi_5, st->wifi_strongest);
    }
    for (int i = 0; i < SENTINEL_AP_MAX; i++) {
        lv_obj_t *row = s_ui.ap_row[i];
        if (i >= st->ap_n) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        const sentinel_ap_t *a = &st->ap[i];
        lv_label_set_text(lv_obj_get_child(row, 0), a->ssid);
        lv_label_set_text_fmt(lv_obj_get_child(row, 1), "CH %-3d", a->ch);
        lv_label_set_text_fmt(lv_obj_get_child(row, 2), "%4d", a->rssi);
    }
}

static void render_sub(void)
{
    const sentinel_state_t *st = &s_ui.st;
    int best = -127, best_i = 0, peak = -127, peak_i = 0;
    for (int i = 0; i < SENTINEL_SUB_CH; i++) {
        int v = st->sub[i], p = st->sub_peak[i];
        /* -120..-30 dBm onto 0..100 */
        int vv = (v - SENTINEL_SUB_FLOOR_DBM) * 100 / 90, pv = (p - SENTINEL_SUB_FLOOR_DBM) * 100 / 90;
        lv_chart_set_series_value_by_id(s_ui.sub_now, s_ui.sub_now_s, (uint32_t)i, vv < 0 ? 0 : vv > 100 ? 100 : vv);
        lv_chart_set_series_value_by_id(s_ui.sub_peak, s_ui.sub_peak_s, (uint32_t)i, pv < 0 ? 0 : pv > 100 ? 100 : pv);
        if (v > best) best = v, best_i = i;
        if (p > peak) peak = p, peak_i = i;
    }
    lv_chart_refresh(s_ui.sub_now);
    lv_chart_refresh(s_ui.sub_peak);
    if (!st->sub_sweeps) {
        lv_label_set_text(s_ui.sub_status, "NO SWEEP YET");
        return;
    }
    lv_label_set_text_fmt(s_ui.sub_status, "NOW %d dBm @ %.1f MHZ    PEAK %d dBm @ %.1f MHZ    SWEEPS %u", best,
                          (double)(SENTINEL_SUB_FIRST_MHZ + best_i * SENTINEL_SUB_STEP_MHZ), peak,
                          (double)(SENTINEL_SUB_FIRST_MHZ + peak_i * SENTINEL_SUB_STEP_MHZ), (unsigned)st->sub_sweeps);
    lv_obj_set_style_text_color(s_ui.sub_status, best >= -80 ? g_pal.warn : g_pal.dim, 0);
}

static void render_nrf(void)
{
    const sentinel_state_t *st = &s_ui.st;
    int busiest = 0, busiest_i = 0, active = 0;
    for (int i = 0; i < SENTINEL_NRF_CH; i++) {
        lv_chart_set_series_value_by_id(s_ui.nrf_now, s_ui.nrf_now_s, (uint32_t)i, st->nrf[i]);
        lv_chart_set_series_value_by_id(s_ui.nrf_peak, s_ui.nrf_peak_s, (uint32_t)i, st->nrf_peak[i]);
        if (st->nrf[i] > busiest) busiest = st->nrf[i], busiest_i = i;
        if (st->nrf[i]) active++;
    }
    lv_chart_refresh(s_ui.nrf_now);
    lv_chart_refresh(s_ui.nrf_peak);
    if (!st->nrf_sweeps) {
        lv_label_set_text(s_ui.nrf_status, "NO SWEEP YET (ONE EVERY 8 PASSES)");
        return;
    }
    lv_label_set_text_fmt(s_ui.nrf_status, "BUSIEST %d MHZ (%d/%d)    %d OF %d CHANNELS ACTIVE    SWEEPS %u",
                          SENTINEL_NRF_FIRST_MHZ + busiest_i, busiest, SENTINEL_NRF_MAX_HITS, active, SENTINEL_NRF_CH,
                          (unsigned)st->nrf_sweeps);
    lv_obj_set_style_text_color(s_ui.nrf_status, busiest >= 6 ? g_pal.warn : g_pal.dim, 0);
}

static void render_events(void)
{
    const sentinel_state_t *st = &s_ui.st;
    uint32_t now = sentinel_now_ms();
    for (int i = 0; i < EVT_ROWS; i++) {
        const sentinel_event_t *e = sentinel_event(st, i);
        if (!e) {
            lv_label_set_text(s_ui.evt_row[i], i == 0 && !st->evt_count ? "NOTHING YET" : "");
            lv_obj_set_style_text_color(s_ui.evt_row[i], g_pal.dim, 0);
            continue;
        }
        unsigned age = (unsigned)((now - e->ms) / 1000);
        char     when[12];
        if (age < 60) snprintf(when, sizeof(when), "%3us", age);
        else if (age < 3600) snprintf(when, sizeof(when), "%3um", age / 60);
        else snprintf(when, sizeof(when), "%3uh", age / 3600);
        lv_label_set_text_fmt(s_ui.evt_row[i], "%s  %-5s %s", when, e->src, e->text);
        lv_obj_set_style_text_color(s_ui.evt_row[i], e->warn ? g_pal.warn : g_pal.text, 0);
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    sentinel_snapshot(&s_ui.st);
    render_link();
    render_gps();
    render_wifi();
    render_sub();
    render_nrf();
    render_events();
    render_hint();
}

/* ---- Screen -------------------------------------------------------------- */

static void build_left(lv_obj_t *col)
{
    /* LINK */
    lv_obj_t *p = rf_panel(col, "LINK");
    lv_obj_t *head = deck_box(p);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);
    s_ui.link_word = deck_label(head, g_font.disp_m, g_pal.dim, "");
    lv_obj_set_flex_grow(s_ui.link_word, 1);
    for (int i = 0; i < CHIP_N; i++) s_ui.chip[i] = deck_chip(head, s_chip_name[i], g_pal.dim);
    s_ui.link_line = deck_label(p, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_width(s_ui.link_line, LV_PCT(100));
    lv_label_set_long_mode(s_ui.link_line, LV_LABEL_LONG_MODE_WRAP);
    s_ui.lines = deck_label(p, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_width(s_ui.lines, LV_PCT(100));
    lv_label_set_long_mode(s_ui.lines, LV_LABEL_LONG_MODE_WRAP);
    s_ui.pins    = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_ui.session = deck_label(p, g_font.mono_s, g_pal.dim, "");

    /* GNSS */
    p = rf_panel(col, "GNSS");
    lv_obj_t *row = deck_box(p);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    s_ui.gps_sats = deck_label(row, g_font.disp_l, g_pal.text, "0");
    lv_obj_t *sats = deck_label(row, g_font.mono_m, g_pal.dim, "SATS USED");
    lv_obj_set_flex_grow(sats, 1);
    s_ui.gps_fix = deck_chip(row, "NO FIX", g_pal.warn);
    s_ui.gps_q   = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_ui.gps_pos = deck_label(p, g_font.mono_m, g_pal.text, "");
    s_ui.gps_alt = deck_label(p, g_font.mono_s, g_pal.dim, "");

    /* WI-FI as the C5 hears it */
    p = rf_panel(col, "WI-FI FROM THE C5");
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_style_pad_row(p, 4, 0);
    s_ui.wifi_sum = deck_label(p, g_font.mono_s, g_pal.dim, "");
    lv_obj_set_width(s_ui.wifi_sum, LV_PCT(100));
    lv_label_set_long_mode(s_ui.wifi_sum, LV_LABEL_LONG_MODE_DOTS);
    for (int i = 0; i < SENTINEL_AP_MAX; i++) {
        lv_obj_t *r = deck_box(p);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 10, 0);
        lv_obj_t *name = deck_label(r, g_font.mono_s, g_pal.text, "");
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(name, 1);
        deck_label(r, g_font.mono_s, g_pal.accent2, "");
        deck_label(r, g_font.mono_s, g_pal.text, "");
        lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
        s_ui.ap_row[i] = r;
    }
}

static void build_right(lv_obj_t *col)
{
    static const char *const sub_ticks[] = {"433.0", "433.5", "434.0", "434.5", "435.0 MHZ"};
    static const char *const nrf_ticks[] = {"2400", "2425", "2450", "2475", "2500", "2525 MHZ"};

    lv_obj_t *p   = rf_panel(col, "SUB-GHZ SWEEP  433.0 TO 435.0 MHZ  21 CHANNELS");
    lv_obj_t *box = chart_box(p, sub_ticks, 5);
    s_ui.sub_peak = bars(box, SENTINEL_SUB_CH, 100, g_pal.accent2, LV_OPA_40, &s_ui.sub_peak_s);
    s_ui.sub_now  = bars(box, SENTINEL_SUB_CH, 100, g_pal.accent, LV_OPA_COVER, &s_ui.sub_now_s);
    s_ui.sub_status = deck_label(p, g_font.mono_s, g_pal.dim, "");

    p   = rf_panel(col, "2.4 GHZ OCCUPANCY  CARRIER DETECT  126 CHANNELS");
    box = chart_box(p, nrf_ticks, 6);
    s_ui.nrf_peak = bars(box, SENTINEL_NRF_CH, SENTINEL_NRF_MAX_HITS, g_pal.accent2, LV_OPA_40, &s_ui.nrf_peak_s);
    s_ui.nrf_now  = bars(box, SENTINEL_NRF_CH, SENTINEL_NRF_MAX_HITS, g_pal.accent, LV_OPA_COVER, &s_ui.nrf_now_s);
    s_ui.nrf_status = deck_label(p, g_font.mono_s, g_pal.dim, "");

    p = rf_panel(col, "EVENTS");
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_style_pad_row(p, 4, 0);
    for (int i = 0; i < EVT_ROWS; i++) {
        s_ui.evt_row[i] = deck_label(p, g_font.mono_s, g_pal.dim, "");
        lv_obj_set_width(s_ui.evt_row[i], LV_PCT(100));
        lv_label_set_long_mode(s_ui.evt_row[i], LV_LABEL_LONG_MODE_DOTS);
    }
}

void rf_overview_build(lv_obj_t *parent)
{
    memset(&s_ui, 0, sizeof(s_ui));

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
    lv_obj_set_size(left, 470, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);
    build_left(left);

    lv_obj_t *right = deck_box(cols);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 10, 0);
    build_right(right);

    s_ui.hint = deck_label(root, g_font.mono_s, g_pal.dim, "");

    s_ui.timer = lv_timer_create(tick, TICK_MS, NULL);
    tick(s_ui.timer);
}

void rf_overview_destroy(void)
{
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    memset(&s_ui, 0, sizeof(s_ui));
}

bool rf_overview_key(const deck_key_t *k)
{
    if (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT)) return false;
    if (k->code == HID_H) return sentinel_send("CMD,HELLO"), true;
    if (k->code == HID_P) return sentinel_send("CMD,PING"), true;
    /* Knock on the transmit gate: the node's answer (ERR,not armed in SAFE,
     * ERR,no such test in RED TEAM) shows up in EVENTS. */
    if (k->code == HID_LETTER('T')) return sentinel_send("CMD,TX,PROBE"), true;
    return false;
}
