/*
 * RF WATCH > SUB-GHZ and RF WATCH > 2.4 GHZ: the two bands the board has
 * radios of its own for, as spectrum analyzers with history. They share
 * this file because they share the chart: the latest sweep as bars with a
 * peak-hold mark, and under it a waterfall, newest row on top.
 *
 *   SUB-GHZ   1 SPECTRUM  433.0 to 435.0 MHz from the CC1101 sweep, a
 *                         minute of history.
 *             2 BURSTS    the CC1101 parked on one frequency, timing what
 *                         transmits there (sentinel_burst.h). While this
 *                         view is up there is no 433 MHz sweep and no
 *                         433 MHz alert; leaving it hands the radio back.
 *   2.4 GHZ   2400 to 2525 MHz from the nRF24 (carrier seen or not, 0 to 8
 *             times in 8 sweeps), two minutes of history, with where Wi-Fi
 *             channels 1, 6 and 11 and the Bluetooth advertising channels
 *             sit, and which of the three carries the least.
 *
 * The history, the statistics and the jam watch live in the driver and run
 * whatever is on screen (deck_sentinel.h); this file draws a copy. S runs a
 * site survey (a minute of both bands, then a file), B tells the node that
 * what it hears now is normal.
 */
#include "rf_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TICK_MS    500
#define BARS_H     150
#define LABEL_H    22
#define SURVEY_MS  60000
#define BURST_ROWS 14
#define EM_ROWS    6
#define KHZ_STEP   10

typedef enum { BAND_SUB, BAND_NRF } band_t;
typedef enum { VIEW_SPECTRUM, VIEW_BURSTS } view_t;

static struct {
    band_t      band;
    view_t      view;
    lv_obj_t   *content, *tab[2], *hint, *chart;
    lv_obj_t   *state, *line[3], *jam, *site, *sel_head, *sel[3], *load[4];
    lv_obj_t   *em[EM_ROWS], *row[BURST_ROWS];
    lv_timer_t *timer;
    int         sel_sub, sel_nrf;
    uint32_t    khz;       /* where BURSTS listens */
    bool        surveying, site_failed;
    uint32_t    survey_end_ms, learn_ms;
    char        site_file[32];
    sentinel_state_t    st;
    sentinel_bandhost_t b;
} s_bd DECK_PSRAM_BSS;

static void set_view(view_t v);

/* ---- The chart ------------------------------------------------------------ */

static lv_color_t heat(uint8_t v)
{
    if (v == 0) return g_pal.panel;
    if (v < 210) return lv_color_mix(g_pal.accent, g_pal.panel, (uint8_t)(12 + v * 243 / 210));
    return lv_color_mix(lv_color_white(), g_pal.accent, (uint8_t)((v - 210) * 5));
}

static void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa   = LV_OPA_COVER;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

static void text(lv_layer_t *layer, int32_t x, int32_t y, const char *s, lv_color_t c)
{
    lv_draw_label_dsc_t t;
    lv_draw_label_dsc_init(&t);
    t.text       = s;
    t.text_local = true; /* the string is on the caller's stack */
    t.font       = g_font.mono_s;
    t.color      = c;
    lv_area_t a  = {x, y, x + 90, y + LABEL_H};
    lv_draw_label(layer, &t, &a);
}

/* Bars with peak marks, an axis, and the waterfall under them. One function
 * for both bands: `n` channels, intensities 0 to 254. */
static void chart_draw(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);
    bool    nrf = s_bd.band == BAND_NRF;
    int     n   = nrf ? SENTINEL_NRF_CH : SENTINEL_SUB_CH, sel = nrf ? s_bd.sel_nrf : s_bd.sel_sub;
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t cw = w / n, gut = nrf ? 1 : 3; /* a column and the gap inside it */
    int32_t top = a.y1 + (nrf ? 26 : 0);   /* room for the Wi-Fi and Bluetooth marks */
    int32_t fall_y = top + BARS_H + LABEL_H + 6, rh = (a.y2 - fall_y) / SENTINEL_BANDFALL_ROWS;
    if (cw < 2 || rh < 2 || h < BARS_H + LABEL_H + 60) return;
    const sentinel_state_t *st = &s_bd.st;

    if (nrf) {
        /* Where the usual tenants are: Wi-Fi 1, 6 and 11 (22 MHz each) and the three Bluetooth advertising channels. */
        static const int wifi[] = {12, 37, 62}, ble[] = {2, 26, 80};
        static const char *const name[] = {"1", "6", "11"};
        for (int i = 0; i < 3; i++) {
            fill(layer, a.x1 + (wifi[i] - 11) * cw, a.y1 + 21, a.x1 + (wifi[i] + 11) * cw, a.y1 + 22, g_pal.dim);
            text(layer, a.x1 + wifi[i] * cw - 6, a.y1, name[i], g_pal.dim);
            fill(layer, a.x1 + ble[i] * cw, a.y1 + 14, a.x1 + ble[i] * cw + cw - 1, a.y1 + 22, g_pal.warn);
        }
    }
    for (int i = 0; i < n; i++) {
        int32_t x = a.x1 + i * cw;
        uint8_t v = nrf ? sentinel_nrf_intensity(st->nrf[i]) : sentinel_sub_intensity(st->sub[i]);
        uint8_t p = nrf ? sentinel_nrf_intensity(st->nrf_peak[i]) : sentinel_sub_intensity(st->sub_peak[i]);
        fill(layer, x, top, x + cw - gut, top + BARS_H, i == sel ? g_pal.panel_hi : g_pal.panel);
        int32_t bh = (int32_t)v * BARS_H / 254, ph = (int32_t)p * BARS_H / 254;
        if (bh > 0) fill(layer, x, top + BARS_H - bh, x + cw - gut, top + BARS_H, heat(v < 60 ? 60 : v));
        if (ph > bh + 2) fill(layer, x, top + BARS_H - ph, x + cw - gut, top + BARS_H - ph + 1, g_pal.accent2);
    }
    /* The axis: a frequency every half megahertz, or every 25. */
    for (int i = 0; i < n; i += nrf ? 25 : 5) {
        char f[12];
        if (nrf) snprintf(f, sizeof(f), "%d", SENTINEL_NRF_FIRST_MHZ + i);
        else snprintf(f, sizeof(f), "%.1f", (double)(SENTINEL_SUB_FIRST_MHZ + SENTINEL_SUB_STEP_MHZ * (float)i));
        text(layer, a.x1 + i * cw, top + BARS_H + 3, f, g_pal.dim);
    }
    /* History, newest on top. Neighbours of one colour are one rectangle, and empty is not drawn at all. */
    for (int r = 0; r < SENTINEL_BANDFALL_ROWS; r++) {
        const uint8_t *row = nrf ? sentinel_bandhost_nrf_row(&s_bd.b, r) : sentinel_bandhost_sub_row(&s_bd.b, r);
        if (!row) break;
        int32_t y = fall_y + r * rh;
        for (int i = 0; i < n;) {
            int j = i + 1;
            if (nrf)
                while (j < n && row[j] == row[i]) j++;
            if (row[i]) fill(layer, a.x1 + i * cw, y, a.x1 + j * cw - (nrf ? 0 : gut), y + rh - 1, heat(row[i]));
            i = j;
        }
    }
}

/* ---- Text ----------------------------------------------------------------- */

static void fmt_ago(char *out, size_t n, uint32_t ms)
{
    if (ms < 60000) snprintf(out, n, "%2us", (unsigned)(ms / 1000));
    else snprintf(out, n, "%2um", (unsigned)(ms / 60000));
}

static void fmt_span(char *out, size_t n, uint32_t ms)
{
    if (ms < 1000) snprintf(out, n, "%u MS", (unsigned)ms);
    else if (ms < 60000) snprintf(out, n, "%.1f S", (double)ms / 1000.0);
    else snprintf(out, n, "%u MIN", (unsigned)(ms / 60000));
}

static void jam_and_site_lines(void)
{
    const sentinel_jam_t *j = &s_bd.b.jam;
    lv_label_set_text_fmt(s_bd.jam, "JAM WATCH  %s", j->text[0] ? j->text : "NONE SEEN");
    lv_obj_set_style_text_color(s_bd.jam, j->level == SENTINEL_JAM_LIKELY ? g_pal.danger : j->level == SENTINEL_JAM_POSSIBLE ? g_pal.warn : g_pal.ok, 0);
    uint32_t now = sentinel_now_ms();
    if (s_bd.learn_ms && now - s_bd.learn_ms < 4000) {
        lv_label_set_text(s_bd.site, "THE NODE IS LEARNING BOTH BANDS AGAIN (ABOUT 40 S)");
    } else if (s_bd.surveying) {
        lv_label_set_text_fmt(s_bd.site, "SITE SURVEY  %u S LEFT", (unsigned)((s_bd.survey_end_ms - now + 999) / 1000));
    } else if (s_bd.site_failed) {
        lv_label_set_text(s_bd.site, "SITE SURVEY  NOTHING WRITTEN (NO SWEEPS, OR NO STORAGE)");
    } else if (s_bd.site_file[0]) {
        lv_label_set_text_fmt(s_bd.site, "SITE SURVEY  WRITTEN: sentinel/%s", s_bd.site_file);
    } else {
        lv_label_set_text(s_bd.site, "SITE SURVEY  [S] A MINUTE OF BOTH BANDS, THEN A FILE");
    }
}

static void render_sub(void)
{
    const sentinel_state_t *st = &s_bd.st;
    bool link = sentinel_link(st);
    int  best = 0;
    for (int i = 1; i < SENTINEL_SUB_CH; i++)
        if (st->sub[i] > st->sub[best]) best = i;
    /* The floor: the median channel of the latest sweep. */
    int8_t sorted[SENTINEL_SUB_CH];
    memcpy(sorted, st->sub, sizeof(sorted));
    for (int i = 1; i < SENTINEL_SUB_CH; i++)
        for (int k = i; k > 0 && sorted[k] < sorted[k - 1]; k--) {
            int8_t t = sorted[k];
            sorted[k] = sorted[k - 1], sorted[k - 1] = t;
        }
    if (!link || !st->cc_ok) {
        lv_label_set_text(s_bd.state, !link ? "NO LINK" : "NO CC1101");
        lv_obj_set_style_text_color(s_bd.state, g_pal.warn, 0);
    } else if (st->sub_band == SENTINEL_BAND_UP) {
        lv_label_set_text_fmt(s_bd.state, "BAND UP %d DB", (int)st->sub_band_db);
        lv_obj_set_style_text_color(s_bd.state, g_pal.danger, 0);
    } else if (st->sub_band == SENTINEL_BAND_LOW) {
        lv_label_set_text_fmt(s_bd.state, "BAND QUIET %d DB", (int)st->sub_band_db);
        lv_obj_set_style_text_color(s_bd.state, g_pal.warn, 0);
    } else {
        lv_label_set_text(s_bd.state, "NORMAL");
        lv_obj_set_style_text_color(s_bd.state, g_pal.ok, 0);
    }
    lv_label_set_text_fmt(s_bd.line[0], "FLOOR %d dBm    STRONGEST %d dBm @ %.1f MHZ", (int)sorted[SENTINEL_SUB_CH / 2], (int)st->sub[best],
                          (double)(SENTINEL_SUB_FIRST_MHZ + SENTINEL_SUB_STEP_MHZ * (float)best));
    if (st->sub_band != SENTINEL_BAND_NORMAL)
        lv_label_set_text_fmt(s_bd.line[1], "%u OF %d CHANNELS OFF THEIR BASELINE. NOTHING IS LEARNED UNTIL IT ENDS, OR [B].", (unsigned)st->sub_band_ch, SENTINEL_SUB_CH);
    else lv_label_set_text_fmt(s_bd.line[1], "SWEEPS %u    21 CHANNELS, 433.0 TO 435.0 MHZ", (unsigned)st->sub_sweeps);
    lv_label_set_text(s_bd.line[2], st->sub_ok ? "" : "THIS NODE'S FIRMWARE HAS NO BAND WATCH (NEEDS 0.7.0)");
    jam_and_site_lines();

    int                       i = s_bd.sel_sub;
    const sentinel_substat_t *c = &s_bd.b.sub_stat[i];
    lv_label_set_text_fmt(s_bd.sel_head, "%.1f MHZ    NOW %d dBm", (double)(SENTINEL_SUB_FIRST_MHZ + SENTINEL_SUB_STEP_MHZ * (float)i), (int)st->sub[i]);
    if (c->n) {
        lv_label_set_text_fmt(s_bd.sel[0], "FLOOR %d    MEAN %.1f    PEAK %d dBm", (int)c->floor, (double)(c->sum / (float)c->n), (int)c->peak);
        lv_label_set_text_fmt(s_bd.sel[1], "BUSY %.1f %% OF %u SWEEPS", 100.0 * (double)c->busy / (double)c->n, (unsigned)c->n);
    } else {
        lv_label_set_text(s_bd.sel[0], "NO SWEEPS YET");
        lv_label_set_text(s_bd.sel[1], "");
    }
    lv_label_set_text(s_bd.sel[2], "BUSY: 8 DB OVER THE CHANNEL'S OWN RUNNING LEVEL. A SWEEP VISITS EACH CHANNEL FOR A MILLISECOND, SO SHORT "
                                   "BURSTS ARE MISSED HERE: USE 2 BURSTS.");
}

static void render_nrf(void)
{
    const sentinel_state_t *st = &s_bd.st;
    bool link = sentinel_link(st);
    int  active = 0, solid = 0, best = 0;
    for (int i = 0; i < SENTINEL_NRF_CH; i++) {
        if (st->nrf[i]) active++;
        if (st->nrf[i] >= SENTINEL_NRF_SAT_HITS) solid++;
        if (st->nrf[i] > st->nrf[best]) best = i;
    }
    if (!link || !st->nrf_ok) {
        lv_label_set_text(s_bd.state, !link ? "NO LINK" : "NO NRF24");
        lv_obj_set_style_text_color(s_bd.state, g_pal.warn, 0);
    } else if (st->nrf_band == SENTINEL_BAND_UP) {
        lv_label_set_text_fmt(s_bd.state, "SATURATED  %u CH", (unsigned)st->nrf_band_ch);
        lv_obj_set_style_text_color(s_bd.state, g_pal.danger, 0);
    } else {
        lv_label_set_text(s_bd.state, "NORMAL");
        lv_obj_set_style_text_color(s_bd.state, g_pal.ok, 0);
    }
    lv_label_set_text_fmt(s_bd.line[0], "ACTIVE %d OF %d    SOLID %d    BUSIEST %d MHZ (%u/8)", active, SENTINEL_NRF_CH, solid, SENTINEL_NRF_FIRST_MHZ + best,
                          (unsigned)st->nrf[best]);
    lv_label_set_text_fmt(s_bd.line[1], "REPORTS %u    SATURATED MEANS %d CHANNELS SOLID: BUSY WI-FI DOES NOT COME NEAR", (unsigned)st->nrf_sweeps, SENTINEL_NRF_SAT_CH);
    lv_label_set_text(s_bd.line[2], st->sub_ok ? "" : "THIS NODE'S FIRMWARE HAS NO BAND WATCH (NEEDS 0.7.0)");
    jam_and_site_lines();

    /* Which of the three channels that do not overlap carries the least, measured. */
    static const int chans[3] = {1, 6, 11};
    float load[3];
    int   quiet = 0;
    for (int k = 0; k < 3; k++) {
        load[k] = sentinel_bandhost_wifi_load(&s_bd.b, chans[k]);
        if (load[k] < load[quiet]) quiet = k;
        lv_label_set_text_fmt(s_bd.load[k], "CH %-2d  %d MHZ    %.1f OF 8", chans[k], 2407 + 5 * chans[k], (double)load[k]);
    }
    for (int k = 0; k < 3; k++) lv_obj_set_style_text_color(s_bd.load[k], k == quiet && s_bd.b.nrf_stat[0].n ? g_pal.ok : g_pal.text, 0);
    if (s_bd.b.nrf_stat[0].n) lv_label_set_text_fmt(s_bd.load[3], "QUIETEST BY ENERGY: CHANNEL %d  (OVER %u REPORTS)", chans[quiet], (unsigned)s_bd.b.nrf_stat[0].n);
    else lv_label_set_text(s_bd.load[3], "NO REPORTS YET");

    int                       i = s_bd.sel_nrf;
    const sentinel_nrfstat_t *c = &s_bd.b.nrf_stat[i];
    lv_label_set_text_fmt(s_bd.sel_head, "%d MHZ    NOW %u OF 8", SENTINEL_NRF_FIRST_MHZ + i, (unsigned)st->nrf[i]);
    if (c->n) {
        lv_label_set_text_fmt(s_bd.sel[0], "MEAN %.1f    PEAK %u OF 8", (double)c->sum / (double)c->n, (unsigned)c->peak);
        lv_label_set_text_fmt(s_bd.sel[1], "A CARRIER IN %.0f %% OF %u REPORTS", 100.0 * (double)c->any / (double)c->n, (unsigned)c->n);
    } else {
        lv_label_set_text(s_bd.sel[0], "NO REPORTS YET");
        lv_label_set_text(s_bd.sel[1], "");
    }
    lv_label_set_text(s_bd.sel[2], "");
}

static void render_bursts(void)
{
    const sentinel_state_t     *st = &s_bd.st;
    const sentinel_bursthost_t *bh = &s_bd.b.bursts;
    bool on = st->sub_dwell_khz == s_bd.khz && sentinel_link(st);
    lv_label_set_text(s_bd.state, !st->sub_ok ? "NOT ON THIS NODE" : on ? "LISTENING" : "ASKING THE NODE");
    lv_obj_set_style_text_color(s_bd.state, !st->sub_ok ? g_pal.warn : on ? g_pal.ok : g_pal.dim, 0);
    lv_label_set_text_fmt(s_bd.line[0], "%u.%03u MHZ    FLOOR %d dBm    BURSTS %u", (unsigned)(s_bd.khz / 1000), (unsigned)(s_bd.khz % 1000),
                          on ? (int)st->sub_dwell_floor : 0, (unsigned)bh->total);
    lv_label_set_text(s_bd.line[1], st->sub_ok ? "THE CC1101 IS PARKED HERE: NO 433 MHZ SWEEP AND NO 433 MHZ ALERTS UNTIL YOU LEAVE THIS VIEW."
                                               : "THE BURST LISTENER NEEDS NODE FIRMWARE 0.7.0.");
    lv_label_set_text(s_bd.line[2], "");

    uint32_t now = sentinel_now_ms();
    int      r   = 0;
    for (int i = 0; i < SENTINEL_EMITTERS_MAX && r < EM_ROWS; i++) {
        const sentinel_emitter_t *e = &bh->em[i];
        if (!e->used || e->count < 2) continue; /* once is not an emitter yet */
        char len[16], per[24] = "NO RHYTHM YET";
        fmt_span(len, sizeof(len), e->ms);
        if (e->regular) {
            char p[16];
            fmt_span(p, sizeof(p), e->period_ms);
            snprintf(per, sizeof(per), "EVERY %s", p);
        }
        lv_label_set_text_fmt(s_bd.em[r], "x%-3u %-8s %4d dBm   %s", (unsigned)e->count, len, (int)e->peak, per);
        lv_obj_set_style_text_color(s_bd.em[r], e->regular ? g_pal.accent : g_pal.text, 0);
        r++;
    }
    for (; r < EM_ROWS; r++) lv_label_set_text(s_bd.em[r], r == 0 ? "NOTHING HAS REPEATED YET" : "");

    for (int k = 0; k < BURST_ROWS; k++) {
        const sentinel_burst_seen_t *b = sentinel_bursthost_get(bh, k);
        if (!b) {
            lv_label_set_text(s_bd.row[k], k == 0 ? "NOTHING HEARD YET" : "");
            lv_obj_set_style_text_color(s_bd.row[k], g_pal.dim, 0);
            continue;
        }
        char ago[8], len[16], gap[16] = "";
        fmt_ago(ago, sizeof(ago), now - b->at_ms);
        fmt_span(len, sizeof(len), b->b.ms);
        if (b->b.gap_ms) fmt_span(gap, sizeof(gap), b->b.gap_ms);
        lv_label_set_text_fmt(s_bd.row[k], "%s  %-9s %4d dBm  %-9s %s%s", ago, len, (int)b->b.peak, gap, sentinel_burst_kind(&b->b), b->b.ongoing ? " (STILL UP)" : "");
        lv_obj_set_style_text_color(s_bd.row[k], b->b.ongoing ? g_pal.warn : g_pal.text, 0);
    }
}

static void render(void)
{
    if (!s_bd.content) return;
    sentinel_snapshot(&s_bd.st);
    sentinel_band_copy(&s_bd.b);
    if (s_bd.band == BAND_NRF) render_nrf();
    else if (s_bd.view == VIEW_BURSTS) render_bursts();
    else render_sub();
    if (s_bd.chart) lv_obj_invalidate(s_bd.chart);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (s_bd.surveying && (int32_t)(sentinel_now_ms() - s_bd.survey_end_ms) >= 0) {
        s_bd.surveying   = false;
        s_bd.site_failed = !sentinel_site_write(s_bd.site_file, sizeof(s_bd.site_file));
    }
    render();
}

/* ---- Building ------------------------------------------------------------- */

static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = deck_label(parent, font, color, "");
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *one_line(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = deck_label(parent, font, g_pal.dim, "");
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(font) + 6);
    return l;
}

static void tab_clicked(lv_event_t *e) { set_view((view_t)(intptr_t)lv_event_get_user_data(e)); }

static void set_view(view_t v)
{
    bool nrf = s_bd.band == BAND_NRF;
    if (nrf) v = VIEW_SPECTRUM;
    /* The listener belongs to its view: the CC1101 sweeps again the moment it is left. */
    sentinel_band_listen(!nrf && v == VIEW_BURSTS ? s_bd.khz : 0);
    if (!nrf && v == VIEW_BURSTS) s_bd.surveying = false; /* and with no sweep there is nothing to survey */
    s_bd.view = v;
    for (int i = 0; i < 2; i++)
        if (s_bd.tab[i]) lv_obj_set_style_text_color(lv_obj_get_child(s_bd.tab[i], 0), i == (int)v ? g_pal.accent : g_pal.dim, 0);
    lv_label_set_text(s_bd.hint, nrf ? "[LEFT/RIGHT] CHANNEL   [S] SITE SURVEY   [B] LEARN THIS AS NORMAL   [ESC] TOOLS      YELLOW MARKS: BLUETOOTH ADVERTISING"
                                 : v == VIEW_BURSTS ? "[1-2] VIEW   [LEFT/RIGHT] TUNE 10 KHZ   [ENTER] BACK TO 433.92   [ESC] TOOLS      A SHAPE IS A GUESS, NOT A DECODE"
                                                    : "[1-2] VIEW   [LEFT/RIGHT] CHANNEL   [S] SITE SURVEY   [B] LEARN THIS AS NORMAL   [ESC] TOOLS");
    lv_obj_clean(s_bd.content);
    s_bd.chart = s_bd.jam = s_bd.site = s_bd.sel_head = NULL;
    memset(s_bd.sel, 0, sizeof(s_bd.sel));
    memset(s_bd.load, 0, sizeof(s_bd.load));
    memset(s_bd.em, 0, sizeof(s_bd.em));
    memset(s_bd.row, 0, sizeof(s_bd.row));

    lv_obj_t *left = deck_box(s_bd.content);
    lv_obj_set_size(left, 470, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);
    lv_obj_t *p = rf_panel(left, nrf ? "2.4 GHZ BAND  NRF24" : v == VIEW_BURSTS ? "BURST LISTENER  CC1101" : "433 MHZ BAND  CC1101");
    s_bd.state = deck_label(p, g_font.disp_m, g_pal.dim, "");
    for (int i = 0; i < 3; i++) s_bd.line[i] = wrapped(p, g_font.mono_s, i == 0 ? g_pal.text : g_pal.dim);

    lv_obj_t *right = deck_box(s_bd.content);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);

    if (!nrf && v == VIEW_BURSTS) {
        p = rf_panel(left, "EMITTERS  SHAPES THAT REPEAT");
        lv_obj_set_flex_grow(p, 1);
        for (int i = 0; i < EM_ROWS; i++) s_bd.em[i] = one_line(p, g_font.mono_m);
        p = rf_panel(right, "NEWEST FIRST   AGO  LENGTH  PEAK  QUIET BEFORE  SHAPE");
        lv_obj_set_flex_grow(p, 1);
        lv_obj_set_style_pad_row(p, 2, 0);
        for (int i = 0; i < BURST_ROWS; i++) s_bd.row[i] = one_line(p, g_font.mono_m);
        lv_obj_t *note = wrapped(p, g_font.mono_s, g_pal.dim);
        lv_label_set_text(note, "SHORT: A FEW MILLISECONDS, LIKE A TYRE OR WEATHER SENSOR'S FRAME.  TRAIN: A RUN OF PULSES.  LONG: A REMOTE HELD "
                                "DOWN.  CARRIER: SOMETHING THAT STAYS UP.");
    } else {
        s_bd.jam  = wrapped(p, g_font.mono_s, g_pal.ok);
        s_bd.site = wrapped(p, g_font.mono_s, g_pal.dim);
        if (nrf) {
            p = rf_panel(left, "WI-FI CHANNELS BY ENERGY");
            for (int i = 0; i < 4; i++) s_bd.load[i] = deck_label(p, i == 3 ? g_font.mono_s : g_font.mono_m, i == 3 ? g_pal.dim : g_pal.text, "");
        }
        p = rf_panel(left, "SELECTED");
        lv_obj_set_flex_grow(p, 1);
        s_bd.sel_head = deck_label(p, g_font.mono_m, g_pal.text, "");
        s_bd.sel[0]   = deck_label(p, g_font.mono_s, g_pal.text, "");
        s_bd.sel[1]   = deck_label(p, g_font.mono_s, g_pal.text, "");
        s_bd.sel[2]   = wrapped(p, g_font.mono_s, g_pal.dim);

        p = rf_panel(right, nrf ? "NOW, THEN THE LAST 60 REPORTS  NEWEST ON TOP" : "NOW, THEN THE LAST 60 SECONDS OR SO  NEWEST ON TOP");
        lv_obj_set_flex_grow(p, 1);
        s_bd.chart = deck_box(p);
        lv_obj_set_width(s_bd.chart, LV_PCT(100));
        lv_obj_set_flex_grow(s_bd.chart, 1);
        lv_obj_add_event_cb(s_bd.chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);
    }
    render();
}

static void build(lv_obj_t *parent, band_t band)
{
    memset(&s_bd, 0, sizeof(s_bd));
    s_bd.band    = band;
    s_bd.sel_sub = 9;  /* 433.9 MHz */
    s_bd.sel_nrf = 62; /* Wi-Fi channel 11 */
    s_bd.khz     = SENTINEL_SUB_KHZ_DEFAULT;

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 20, 0);
    lv_obj_set_style_pad_row(root, 10, 0);

    if (band == BAND_SUB) {
        static const char *const name[2] = {"1 SPECTRUM", "2 BURSTS"};
        lv_obj_t *tabs = deck_box(root);
        lv_obj_set_width(tabs, LV_PCT(100));
        lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(tabs, 28, 0);
        for (int i = 0; i < 2; i++) {
            s_bd.tab[i] = deck_box(tabs);
            deck_label(s_bd.tab[i], g_font.disp_s, g_pal.dim, name[i]);
            lv_obj_add_flag(s_bd.tab[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(s_bd.tab[i], 10);
            lv_obj_add_event_cb(s_bd.tab[i], tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }
    s_bd.content = deck_box(root);
    lv_obj_set_width(s_bd.content, LV_PCT(100));
    lv_obj_set_flex_grow(s_bd.content, 1);
    lv_obj_set_flex_flow(s_bd.content, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_bd.content, 16, 0);
    s_bd.hint = deck_label(root, g_font.mono_s, g_pal.dim, "");

    set_view(VIEW_SPECTRUM);
    s_bd.timer = lv_timer_create(tick, TICK_MS, NULL);
}

static void destroy(void)
{
    if (s_bd.timer) lv_timer_delete(s_bd.timer);
    s_bd.timer   = NULL;
    s_bd.content = NULL;
    s_bd.chart   = NULL;
    sentinel_band_listen(0); /* the CC1101 goes back to sweeping */
}

static bool key(const deck_key_t *k)
{
    if (!s_bd.timer || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) return false;
    bool nrf = s_bd.band == BAND_NRF, bursts = !nrf && s_bd.view == VIEW_BURSTS;
    if (!nrf && (k->code == HID_1 || k->code == HID_1 + 1)) {
        set_view(k->code == HID_1 ? VIEW_SPECTRUM : VIEW_BURSTS);
        return true;
    }
    if (k->code == HID_LEFT || k->code == HID_RIGHT) {
        int d = k->code == HID_RIGHT ? 1 : -1;
        if (bursts) {
            uint32_t khz = s_bd.khz + (uint32_t)(d * KHZ_STEP);
            if (khz >= SENTINEL_SUB_KHZ_MIN && khz <= SENTINEL_SUB_KHZ_MAX) s_bd.khz = khz;
            sentinel_band_listen(s_bd.khz);
        } else if (nrf) {
            s_bd.sel_nrf = (s_bd.sel_nrf + d + SENTINEL_NRF_CH) % SENTINEL_NRF_CH;
        } else {
            s_bd.sel_sub = (s_bd.sel_sub + d + SENTINEL_SUB_CH) % SENTINEL_SUB_CH;
        }
        render();
        return true;
    }
    if (bursts) {
        if (k->code != HID_ENTER) return false;
        s_bd.khz = SENTINEL_SUB_KHZ_DEFAULT;
        sentinel_band_listen(s_bd.khz);
        render();
        return true;
    }
    if (k->code == HID_LETTER('S')) {
        if (!s_bd.surveying) {
            sentinel_site_begin();
            s_bd.surveying     = true;
            s_bd.site_failed   = false;
            s_bd.site_file[0]  = 0;
            s_bd.survey_end_ms = sentinel_now_ms() + SURVEY_MS;
        }
        render();
        return true;
    }
    if (k->code == HID_LETTER('B')) {
        sentinel_band_learn();
        s_bd.learn_ms = sentinel_now_ms();
        if (!s_bd.learn_ms) s_bd.learn_ms = 1;
        render();
        return true;
    }
    return false;
}

void rf_sub_build(lv_obj_t *parent) { build(parent, BAND_SUB); }
void rf_sub_destroy(void) { destroy(); }
bool rf_sub_key(const deck_key_t *k) { return key(k); }
void rf_nrf_build(lv_obj_t *parent) { build(parent, BAND_NRF); }
void rf_nrf_destroy(void) { destroy(); }
bool rf_nrf_key(const deck_key_t *k) { return key(k); }
