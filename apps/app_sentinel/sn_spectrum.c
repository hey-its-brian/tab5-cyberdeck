/*
 * SENTINEL > SPECTRUM: the two bands the radio board sweeps, one job to a
 * screen.
 *
 *   433 MHZ       the band now and over the last minute or so
 *   TRANSMITTERS  the CC1101 parked on one frequency, timing what transmits
 *                 there and finding what repeats
 *   2.4 GHZ       the band now and over the last few minutes
 *   SITE SURVEY   a minute of both bands, kept as a named file
 *   JAM WATCH     what the jam watch thinks, and the evidence for it
 *
 * The history, the statistics and the jam watch live in the driver and run
 * whatever is on screen (deck_sentinel.h); these screens draw a copy. A
 * site survey belongs to this file, not to its screen, so it finishes and
 * is written even if the user goes elsewhere while it runs.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_theme.h"

#define TICK_MS    500
#define BARS_H     150
#define LABEL_H    26
#define SURVEY_MS  60000
#define BURST_ROWS 8
#define EM_ROWS    6
#define KHZ_STEP   10

typedef enum { BAND_SUB, BAND_NRF } band_t;

/* One screen of this file is up at a time: they share the copies. */
static struct {
    band_t      band;
    lv_obj_t   *chart, *state, *v[6], *em[EM_ROWS], *row[BURST_ROWS], *bar;
    lv_timer_t *timer;
    int         sel_sub, sel_nrf;
    uint32_t    khz; /* where TRANSMITTERS listens */
    void (*render)(void);
    sentinel_state_t    st;
    sentinel_bandhost_t b;
} s_sp DECK_PSRAM_BSS;

/* The survey outlives its screen. */
static struct {
    bool        on, failed;
    uint32_t    end_ms;
    lv_timer_t *timer;
    char        tag[SENTINEL_SITE_TAG_MAX], file[SENTINEL_SITE_NAME_MAX];
} s_sv;

static void pull(void)
{
    sentinel_snapshot(&s_sp.st);
    sentinel_band_copy(&s_sp.b);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    pull();
    if (s_sp.render) s_sp.render();
    if (s_sp.chart) lv_obj_invalidate(s_sp.chart);
}

static void begin(void (*render)(void))
{
    lv_timer_t *old = s_sp.timer;
    int         a = s_sp.sel_sub, b = s_sp.sel_nrf;
    uint32_t    khz = s_sp.khz;
    if (old) lv_timer_delete(old);
    memset(&s_sp, 0, sizeof(s_sp));
    s_sp.sel_sub = a ? a : 9;  /* 433.9 MHz */
    s_sp.sel_nrf = b ? b : 62; /* Wi-Fi channel 11 */
    s_sp.khz     = khz ? khz : SENTINEL_SUB_KHZ_DEFAULT;
    s_sp.render  = render;
    pull();
    s_sp.timer = lv_timer_create(tick, TICK_MS, NULL);
}

static void end(void)
{
    if (s_sp.timer) lv_timer_delete(s_sp.timer);
    s_sp.timer  = NULL;
    s_sp.render = NULL;
    s_sp.chart  = NULL;
}

/* ---- The chart: bars now, history under them -------------------------------- */

static lv_color_t heat(uint8_t v)
{
    if (v == 0) return g_pal.bg;
    if (v < 210) return lv_color_mix(g_pal.accent, g_pal.bg, (uint8_t)(12 + v * 243 / 210));
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
    t.font       = g_font.mono_m;
    t.color      = c;
    lv_area_t a  = {x, y, x + 110, y + LABEL_H};
    lv_draw_label(layer, &t, &a);
}

static int chart_cols(void) { return s_sp.band == BAND_NRF ? SENTINEL_NRF_CH : SENTINEL_SUB_CH; }

static void chart_draw(lv_event_t *e)
{
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   a;
    lv_obj_get_coords(obj, &a);
    bool    nrf = s_sp.band == BAND_NRF;
    int     n = chart_cols(), sel = nrf ? s_sp.sel_nrf : s_sp.sel_sub;
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    int32_t cw = w / n, gut = nrf ? 1 : 3;
    int32_t top = a.y1 + LABEL_H + 2; /* the cursor's label, and on 2.4 GHz the Wi-Fi channel names */
    int32_t fall_y = top + BARS_H + LABEL_H + 6, rh = (a.y2 - fall_y) / SENTINEL_BANDFALL_ROWS;
    if (cw < 2 || rh < 2 || h < BARS_H + LABEL_H * 2 + 40) return;
    const sentinel_state_t *st = &s_sp.st;

    if (nrf) {
        /* Where the usual tenants are: Wi-Fi channels 1, 6 and 11, each 22 MHz wide. */
        static const int         wifi[] = {12, 37, 62};
        static const char *const name[] = {"WI-FI 1", "WI-FI 6", "WI-FI 11"};
        for (int i = 0; i < 3; i++) {
            fill(layer, a.x1 + (wifi[i] - 11) * cw, top - 3, a.x1 + (wifi[i] + 11) * cw, top - 2, g_pal.label);
            if (sel < wifi[i] - 11 || sel > wifi[i] + 11) text(layer, a.x1 + (wifi[i] - 4) * cw, a.y1, name[i], g_pal.label);
        }
    }
    for (int i = 0; i < n; i++) {
        int32_t x = a.x1 + i * cw;
        uint8_t v = nrf ? sentinel_nrf_intensity(st->nrf[i]) : sentinel_sub_intensity(st->sub[i]);
        uint8_t p = nrf ? sentinel_nrf_intensity(st->nrf_peak[i]) : sentinel_sub_intensity(st->sub_peak[i]);
        int32_t bh = (int32_t)v * BARS_H / 254, ph = (int32_t)p * BARS_H / 254;
        if (bh > 0) fill(layer, x, top + BARS_H - bh, x + cw - gut, top + BARS_H, heat(v < 60 ? 60 : v));
        if (ph > bh + 2) fill(layer, x, top + BARS_H - ph, x + cw - gut, top + BARS_H - ph + 1, g_pal.text); /* the peak it reached */
    }
    fill(layer, a.x1, top + BARS_H, a.x1 + n * cw, top + BARS_H, g_pal.line);
    for (int i = 0; i < n; i += nrf ? 25 : 5) {
        char f[12];
        if (nrf) snprintf(f, sizeof(f), "%d", SENTINEL_NRF_FIRST_MHZ + i);
        else snprintf(f, sizeof(f), "%.1f", (double)(SENTINEL_SUB_FIRST_MHZ + SENTINEL_SUB_STEP_MHZ * (float)i));
        text(layer, a.x1 + i * cw, top + BARS_H + 3, f, g_pal.label);
    }
    /* History, newest on top. Empty is not drawn, so the floor is the background. */
    for (int r = 0; r < SENTINEL_BANDFALL_ROWS; r++) {
        const uint8_t *row = nrf ? sentinel_bandhost_nrf_row(&s_sp.b, r) : sentinel_bandhost_sub_row(&s_sp.b, r);
        if (!row) break;
        int32_t y = fall_y + r * rh;
        for (int i = 0; i < n;) {
            int j = i + 1;
            while (j < n && row[j] == row[i]) j++;
            if (row[i] > (nrf ? 0 : 40)) fill(layer, a.x1 + i * cw, y, a.x1 + j * cw - (nrf ? 0 : gut), y + rh - 1, heat(row[i]));
            i = j;
        }
    }
    /* The cursor: a bright line the whole height, with its frequency on top. */
    int32_t cx = a.x1 + sel * cw + (cw - gut) / 2;
    fill(layer, cx, top, cx + 1, a.y2, lv_color_white());
    char f[16];
    if (nrf) snprintf(f, sizeof(f), "%d MHZ", SENTINEL_NRF_FIRST_MHZ + sel);
    else snprintf(f, sizeof(f), "%.1f MHZ", (double)(SENTINEL_SUB_FIRST_MHZ + SENTINEL_SUB_STEP_MHZ * (float)sel));
    int32_t lx = cx - 44;
    if (lx < a.x1) lx = a.x1;
    if (lx > a.x2 - 100) lx = a.x2 - 100;
    text(layer, lx, a.y1, f, lv_color_white());
}

static void cursor_move(int d)
{
    if (s_sp.band == BAND_NRF) s_sp.sel_nrf = (s_sp.sel_nrf + d + SENTINEL_NRF_CH) % SENTINEL_NRF_CH;
    else s_sp.sel_sub = (s_sp.sel_sub + d + SENTINEL_SUB_CH) % SENTINEL_SUB_CH;
    if (s_sp.render) s_sp.render();
    if (s_sp.chart) lv_obj_invalidate(s_sp.chart);
}
static void cursor_left(void) { cursor_move(-1); }
static void cursor_right(void) { cursor_move(1); }

/* A tap on the chart puts the cursor there. */
static void chart_tapped(lv_event_t *e)
{
    lv_obj_t  *obj = lv_event_get_target_obj(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int n = chart_cols(), cw = lv_area_get_width(&a) / n;
    if (cw < 1) return;
    int i = (p.x - a.x1) / cw;
    i     = i < 0 ? 0 : i >= n ? n - 1 : i;
    if (s_sp.band == BAND_NRF) s_sp.sel_nrf = i;
    else s_sp.sel_sub = i;
    cursor_move(0);
}

/* The two band screens share a layout: the state and the cursor's numbers on
 * the left, the chart on the right. */
static void band_build(lv_obj_t *body, band_t band, void (*render)(void))
{
    begin(render);
    s_sp.band = band;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 20, 0);
    lv_obj_t *left = sn_col(body, 440);
    s_sp.state     = sn_state(left);
    s_sp.v[0]      = sn_value(left);
    s_sp.v[1]      = sn_note(left);
    sn_heading(left, "AT THE CURSOR");
    s_sp.v[2] = sn_value(left);
    s_sp.v[3] = sn_value(left);
    s_sp.v[4] = sn_value(left);

    lv_obj_t *right = sn_col(body, 0);
    s_sp.chart      = lv_obj_create(right);
    lv_obj_remove_style_all(s_sp.chart);
    lv_obj_set_width(s_sp.chart, LV_PCT(100));
    lv_obj_set_flex_grow(s_sp.chart, 1);
    lv_obj_add_flag(s_sp.chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sp.chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_sp.chart, chart_tapped, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_sp.chart, chart_tapped, LV_EVENT_PRESSING, NULL);

    sn_action(HID_LEFT, "<", "CURSOR DOWN", cursor_left);
    sn_action(HID_RIGHT, ">", "CURSOR UP", cursor_right);
    render();
}

/* ---- 433 MHZ ---------------------------------------------------------------- */

static void sub_render(void)
{
    const sentinel_state_t *st = &s_sp.st;
    int best = 0;
    for (int i = 1; i < SENTINEL_SUB_CH; i++)
        if (st->sub[i] > st->sub[best]) best = i;
    int8_t sorted[SENTINEL_SUB_CH]; /* the floor: the median channel of the latest sweep */
    memcpy(sorted, st->sub, sizeof(sorted));
    for (int i = 1; i < SENTINEL_SUB_CH; i++)
        for (int k = i; k > 0 && sorted[k] < sorted[k - 1]; k--) {
            int8_t t = sorted[k];
            sorted[k] = sorted[k - 1], sorted[k - 1] = t;
        }
    if (!st->cc_ok) sn_set(s_sp.state, g_pal.warn, "NO 433 MHZ RADIO");
    else if (st->sub_band == SENTINEL_BAND_UP) sn_set(s_sp.state, g_pal.danger, "!! BAND UP %d DB", (int)st->sub_band_db);
    else if (st->sub_band == SENTINEL_BAND_LOW) sn_set(s_sp.state, g_pal.warn, "! BAND QUIET %d DB", (int)st->sub_band_db);
    else sn_set(s_sp.state, g_pal.ok, "NORMAL");
    sn_set(s_sp.v[0], g_pal.text, "FLOOR %d dBm\nSTRONGEST %d dBm AT %.1f MHZ", (int)sorted[SENTINEL_SUB_CH / 2], (int)st->sub[best],
           (double)(SENTINEL_SUB_FIRST_MHZ + SENTINEL_SUB_STEP_MHZ * (float)best));
    if (st->sub_band != SENTINEL_BAND_NORMAL)
        sn_set(s_sp.v[1], g_pal.warn, "%u of %d channels are off their usual level.", (unsigned)st->sub_band_ch, SENTINEL_SUB_CH);
    else sn_set(s_sp.v[1], g_pal.label, "%s", st->sub_ok ? "21 channels, 433.0 to 435.0 MHz, swept about twice a second." : "This board's firmware has no band watch (needs 0.7.0).");

    int                       i = s_sp.sel_sub;
    const sentinel_substat_t *c = &s_sp.b.sub_stat[i];
    sn_set(s_sp.v[2], g_pal.text, "NOW %d dBm", (int)st->sub[i]);
    if (c->n) {
        sn_set(s_sp.v[3], g_pal.text, "USUAL %d    PEAK %d dBm", (int)c->floor, (int)c->peak);
        sn_set(s_sp.v[4], g_pal.text, "BUSY %.1f %% OF THE TIME", 100.0 * (double)c->busy / (double)c->n);
    } else {
        sn_set(s_sp.v[3], g_pal.label, "NO SWEEPS YET");
        sn_set(s_sp.v[4], g_pal.label, " ");
    }
}

static void sub_build(lv_obj_t *body)
{
    band_build(body, BAND_SUB, sub_render);
    sn_help("The bars are the band now; the white marks are the highest each channel has reached. Under them is the last "
            "minute or so, newest on top.\n\n"
            "BUSY counts a channel as busy when it is 8 dB over its own usual level.\n\n"
            "A sweep visits each channel for a millisecond, so short transmissions are missed here. To time what transmits "
            "on one frequency, use TRANSMITTERS.");
}

/* ---- 2.4 GHZ ---------------------------------------------------------------- */

static void nrf_render(void)
{
    const sentinel_state_t *st = &s_sp.st;
    int active = 0, solid = 0;
    for (int i = 0; i < SENTINEL_NRF_CH; i++) {
        if (st->nrf[i]) active++;
        if (st->nrf[i] >= SENTINEL_NRF_SAT_HITS) solid++;
    }
    if (!st->nrf_ok) sn_set(s_sp.state, g_pal.warn, "NO 2.4 GHZ RADIO");
    else if (st->nrf_band == SENTINEL_BAND_UP) sn_set(s_sp.state, g_pal.danger, "!! SATURATED");
    else sn_set(s_sp.state, g_pal.ok, "NORMAL");
    sn_set(s_sp.v[0], g_pal.text, "%d OF %d CHANNELS IN USE\n%d SOLIDLY", active, SENTINEL_NRF_CH, solid);
    if (st->nrf_band == SENTINEL_BAND_UP) sn_set(s_sp.v[1], g_pal.danger, "Busy Wi-Fi does not do this. See JAM WATCH.");
    else sn_set(s_sp.v[1], g_pal.label, "Saturated means %d or more channels solid at once.", SENTINEL_NRF_SAT_CH);

    int                       i = s_sp.sel_nrf;
    const sentinel_nrfstat_t *c = &s_sp.b.nrf_stat[i];
    sn_set(s_sp.v[2], g_pal.text, "NOW %u %% BUSY", (unsigned)st->nrf[i] * 100u / 8u);
    if (c->n) {
        sn_set(s_sp.v[3], g_pal.text, "AVERAGE %.0f %%    PEAK %u %%", 100.0 * (double)c->sum / (double)c->n / 8.0, (unsigned)c->peak * 100u / 8u);
        sn_set(s_sp.v[4], g_pal.text, "IN USE %.0f %% OF THE TIME", 100.0 * (double)c->any / (double)c->n);
    } else {
        sn_set(s_sp.v[3], g_pal.label, "NO REPORTS YET");
        sn_set(s_sp.v[4], g_pal.label, " ");
    }
}

static void nrf_build(lv_obj_t *body)
{
    band_build(body, BAND_NRF, nrf_render);
    sn_help("The bars are 2400 to 2525 MHz now; the white marks are the highest each channel has reached. Under them are the "
            "last sixty reports (about five minutes), newest on top.\n\n"
            "The lines along the top show where Wi-Fi channels 1, 6 and 11 sit. Bluetooth advertises at 2402, 2426 and 2480 "
            "MHz.\n\n"
            "This radio only tells whether a carrier is there, not how strong it is, so a channel is shown by how much of "
            "the time it was busy.");
}

/* ---- TRANSMITTERS ----------------------------------------------------------- */

static void fmt_len(char *out, size_t n, uint32_t ms)
{
    if (ms < 1000) snprintf(out, n, "%u MS", (unsigned)ms);
    else if (ms < 60000) snprintf(out, n, "%.1f S", (double)ms / 1000.0);
    else snprintf(out, n, "%u MIN", (unsigned)(ms / 60000));
}

static void tx_render(void)
{
    const sentinel_state_t     *st = &s_sp.st;
    const sentinel_bursthost_t *bh = &s_sp.b.bursts;
    bool on = st->sub_dwell_khz == s_sp.khz;
    if (!st->sub_ok) sn_set(s_sp.state, g_pal.warn, "NEEDS BOARD FIRMWARE 0.7.0");
    else sn_set(s_sp.state, on ? g_pal.ok : g_pal.label, "%s %u.%03u MHZ", on ? "LISTENING AT" : "TUNING TO", (unsigned)(s_sp.khz / 1000), (unsigned)(s_sp.khz % 1000));
    sn_set(s_sp.v[0], g_pal.label, "%u heard so far. While this screen is open the 433 MHz band is not swept.", (unsigned)bh->total);

    uint32_t now = sentinel_now_ms();
    int      r   = 0;
    for (int i = 0; i < SENTINEL_EMITTERS_MAX && r < EM_ROWS; i++) {
        const sentinel_emitter_t *e = &bh->em[i];
        if (!e->used || e->count < 2) continue; /* once is not a transmitter yet */
        char len[16], per[28] = "NO RHYTHM YET";
        fmt_len(len, sizeof(len), e->ms);
        if (e->regular) {
            char p[16];
            fmt_len(p, sizeof(p), e->period_ms);
            snprintf(per, sizeof(per), "EVERY %s", p);
        }
        sn_set(s_sp.em[r], g_pal.text, "%-13s %ux %s, %d dBm", per, (unsigned)e->count, len, (int)e->peak);
        r++;
    }
    for (; r < EM_ROWS; r++) sn_set(s_sp.em[r], g_pal.label, "%s", r == 0 ? "NOTHING HAS REPEATED YET" : " ");

    for (int k = 0; k < BURST_ROWS; k++) {
        const sentinel_burst_seen_t *b = sentinel_bursthost_get(bh, k);
        if (!b) {
            sn_set(s_sp.row[k], g_pal.label, "%s", k == 0 ? "NOTHING HEARD YET" : " ");
            continue;
        }
        char ago[24], len[16];
        sn_fmt_span(ago, sizeof(ago), now - b->at_ms);
        fmt_len(len, sizeof(len), b->b.ms);
        sn_set(s_sp.row[k], b->b.ongoing ? g_pal.warn : g_pal.text, "%-7s AGO  %-8s %4d dBm  %s%s", ago, len, (int)b->b.peak, sentinel_burst_kind(&b->b),
               b->b.ongoing ? " (STILL ON)" : "");
    }
}

static void tx_tune(int d)
{
    uint32_t khz = s_sp.khz + (uint32_t)(d * KHZ_STEP);
    if (khz >= SENTINEL_SUB_KHZ_MIN && khz <= SENTINEL_SUB_KHZ_MAX) s_sp.khz = khz;
    sentinel_band_listen(s_sp.khz);
    tx_render();
}
static void tx_down(void) { tx_tune(-1); }
static void tx_up(void) { tx_tune(1); }
static void tx_home(void)
{
    s_sp.khz = SENTINEL_SUB_KHZ_DEFAULT;
    tx_tune(0);
}

static void tx_build(lv_obj_t *body)
{
    begin(tx_render);
    if (s_sv.on) { /* with no sweep there is nothing to survey */
        s_sv.on = false;
        sn_say(SN_WARNING, "SITE SURVEY CANCELLED: THE 433 MHZ RADIO IS NOW LISTENING TO ONE FREQUENCY.", 20000);
    }
    sentinel_band_listen(s_sp.khz);
    s_sp.state = sn_state(body);
    s_sp.v[0]  = sn_note(body);
    lv_obj_t *cols = sn_row_box(body);
    lv_obj_set_flex_grow(cols, 1);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *left = sn_col(cols, 520);
    sn_heading(left, "WHAT REPEATS, AND HOW OFTEN");
    for (int i = 0; i < EM_ROWS; i++) s_sp.em[i] = sn_value(left);
    lv_obj_t *right = sn_col(cols, 0);
    sn_heading(right, "LAST HEARD, NEWEST FIRST");
    for (int i = 0; i < BURST_ROWS; i++) {
        s_sp.row[i] = sn_value(right);
        lv_obj_set_style_text_font(s_sp.row[i], g_font.mono_m, 0);
    }
    sn_action(HID_LEFT, "<", "10 KHZ DOWN", tx_down);
    sn_action(HID_RIGHT, ">", "10 KHZ UP", tx_up);
    sn_action(HID_ENTER, "ENTER", "BACK TO 433.92", tx_home);
    sn_help("The 433 MHz radio stays on one frequency and times every transmission there: how long, how strong, how long "
            "since the last one. Transmissions of the same shape are grouped, and a group that keeps a rhythm gets its "
            "period. That is how a weather station, a remote or a tyre sensor shows up.\n\n"
            "It reads shapes, not contents: nothing is decoded. SHORT is a few milliseconds, like a sensor's frame. TRAIN is "
            "a run of pulses. LONG is a remote held down. CARRIER is something that stays on.\n\n"
            "Changing the frequency starts the lists again. While this screen is open the band is not swept, so there are "
            "no 433 MHz warnings.");
    tx_render();
}

static void tx_destroy(void)
{
    end();
    sentinel_band_listen(0); /* the CC1101 goes back to sweeping */
}

/* ---- SITE SURVEY ------------------------------------------------------------- */

static void sv_render(void);

static void sv_done(lv_timer_t *t)
{
    (void)t;
    if (s_sv.timer) lv_timer_delete(s_sv.timer);
    s_sv.timer = NULL;
    if (!s_sv.on) return;
    s_sv.on     = false;
    s_sv.failed = !sentinel_site_write(s_sv.tag, s_sv.file, sizeof(s_sv.file));
    if (s_sp.render == sv_render) sv_render();
    else sn_say(s_sv.failed ? SN_WARNING : SN_NOTE, s_sv.failed ? "SITE SURVEY: NOTHING WAS WRITTEN." : "SITE SURVEY SAVED. SEE SPECTRUM > SITE SURVEY.", 30000);
}

/* The survey starts when the place has its name: Enter on an empty box is a
 * survey without one, Esc is no survey. The minute is counted from here, so
 * the typing is not part of what was measured. */
static void sv_named(const char *text_in, void *user)
{
    (void)user;
    if (!text_in || s_sv.on) return;
    sentinel_site_tag(s_sv.tag, sizeof(s_sv.tag), text_in);
    sentinel_site_begin();
    s_sv.on      = true;
    s_sv.failed  = false;
    s_sv.file[0] = 0;
    s_sv.end_ms  = sentinel_now_ms() + SURVEY_MS;
    if (s_sv.timer) lv_timer_delete(s_sv.timer);
    s_sv.timer = lv_timer_create(sv_done, SURVEY_MS, NULL);
    lv_timer_set_repeat_count(s_sv.timer, 1);
    lv_timer_set_auto_delete(s_sv.timer, false);
    if (s_sp.render == sv_render) sv_render();
}

static void sv_start(void)
{
    if (!s_sv.on) deck_modal_prompt("NAME THIS PLACE (ENTER FOR NONE, ESC CANCELS)", "", sv_named, NULL);
}

static void sv_cancel_yes(void)
{
    s_sv.on = false;
    if (s_sp.render == sv_render) sv_render();
}

static void sv_cancel(void)
{
    if (s_sv.on) sn_confirm("CANCEL THE SURVEY?", "What it has measured so far is thrown away.", "CANCEL IT", sv_cancel_yes);
}

static void sv_render(void)
{
    uint32_t now = sentinel_now_ms();
    if (s_sv.on) {
        unsigned left = (unsigned)((s_sv.end_ms - now + 999) / 1000);
        if (left > SURVEY_MS / 1000) left = SURVEY_MS / 1000;
        sn_set(s_sp.state, g_pal.accent, "MEASURING, %u S LEFT", left);
        sn_set(s_sp.v[0], g_pal.text, "PLACE: %s", s_sv.tag[0] ? s_sv.tag : "(NO NAME)");
        sn_set(s_sp.v[1], g_pal.label, "You can leave this screen. The survey finishes and is saved by itself.");
        lv_bar_set_value(s_sp.bar, (int32_t)(SURVEY_MS / 1000 - left), LV_ANIM_OFF);
        lv_obj_remove_flag(s_sp.bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sp.bar, LV_OBJ_FLAG_HIDDEN);
        if (s_sv.failed) {
            sn_set(s_sp.state, g_pal.warn, "! NOTHING WAS WRITTEN");
            sn_set(s_sp.v[0], g_pal.text, "Either no sweeps arrived in that minute, or there is no storage.");
            sn_set(s_sp.v[1], g_pal.label, " ");
        } else if (s_sv.file[0]) {
            sn_set(s_sp.state, g_pal.ok, "SAVED");
            sn_set(s_sp.v[0], g_pal.text, "sentinel/%s", s_sv.file);
            sn_set(s_sp.v[1], g_pal.label, "Every channel's usual level, average, peak and busy share, for both bands. Read it in FILES or over the portal.");
        } else {
            sn_set(s_sp.state, g_pal.text, "READY");
            sn_set(s_sp.v[0], g_pal.text, "Measures 433 MHz and 2.4 GHz here for one minute and keeps the result as a file.");
            sn_set(s_sp.v[1], g_pal.label, "You will be asked what to call this place, so the file makes sense later.");
        }
    }
    sn_action_enable(sv_start, !s_sv.on);
    sn_action_enable(sv_cancel, s_sv.on);
}

static void sv_build(lv_obj_t *body)
{
    begin(sv_render);
    s_sp.state = sn_state(body);
    s_sp.bar   = lv_bar_create(body);
    lv_obj_set_size(s_sp.bar, LV_PCT(100), 22);
    lv_bar_set_range(s_sp.bar, 0, SURVEY_MS / 1000);
    lv_obj_set_style_bg_color(s_sp.bar, g_pal.line, 0);
    lv_obj_set_style_bg_color(s_sp.bar, g_pal.accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sp.bar, 4, 0);
    lv_obj_set_style_radius(s_sp.bar, 4, LV_PART_INDICATOR);
    s_sp.v[0] = sn_value(body);
    s_sp.v[1] = sn_note(body);
    sn_action(HID_LETTER('S'), "S", "START A SURVEY", sv_start);
    sn_action(HID_LETTER('C'), "C", "CANCEL IT", sv_cancel);
    sv_render();
}

/* ---- JAM WATCH --------------------------------------------------------------- */

static void jam_render(void)
{
    const sentinel_state_t *st = &s_sp.st;
    const sentinel_jam_t   *j  = &s_sp.b.jam;
    if (j->level == SENTINEL_JAM_LIKELY) sn_set(s_sp.state, g_pal.danger, "!! JAMMING IS LIKELY");
    else if (j->level == SENTINEL_JAM_POSSIBLE) sn_set(s_sp.state, g_pal.warn, "! JAMMING IS POSSIBLE");
    else sn_set(s_sp.state, g_pal.ok, "NO SIGN OF JAMMING");
    sn_set(s_sp.v[0], j->level ? g_pal.text : g_pal.label, "%s", j->text[0] ? j->text : "Both bands look the way they usually do here.");

    if (st->sub_band == SENTINEL_BAND_UP) sn_set(s_sp.v[1], g_pal.danger, "!! 433 MHZ: THE WHOLE BAND IS UP %d DB (%u CHANNELS)", (int)st->sub_band_db, (unsigned)st->sub_band_ch);
    else sn_set(s_sp.v[1], g_pal.text, "433 MHZ: NORMAL");
    if (st->nrf_band == SENTINEL_BAND_UP) sn_set(s_sp.v[2], g_pal.danger, "!! 2.4 GHZ: SATURATED (%u CHANNELS SOLID)", (unsigned)st->nrf_band_ch);
    else sn_set(s_sp.v[2], g_pal.text, "2.4 GHZ: NORMAL");
    if (!j->ble_seeded) sn_set(s_sp.v[3], g_pal.label, "BLUETOOTH: STILL LEARNING WHAT IS USUAL");
    else if (j->ble_down) sn_set(s_sp.v[3], g_pal.warn, "! BLUETOOTH: %u ADVERTS A SECOND, USUALLY %.0f", (unsigned)st->ble_rate, (double)j->ble_avg);
    else sn_set(s_sp.v[3], g_pal.text, "BLUETOOTH: %u ADVERTS A SECOND, AS USUAL", (unsigned)st->ble_rate);
    if (!j->gps_seeded) sn_set(s_sp.v[4], g_pal.label, "GPS: STILL LEARNING WHAT IS USUAL");
    else if (j->gps_down) sn_set(s_sp.v[4], g_pal.warn, "! GPS: %d SATELLITES, USUALLY %.0f", (int)st->sats_used, (double)j->sats_avg);
    else sn_set(s_sp.v[4], g_pal.text, "GPS: %d SATELLITES, AS USUAL", (int)st->sats_used);
}

static void jam_learn_yes(void)
{
    sentinel_band_learn();
    sentinel_note(false, "baseline reset by the user: both bands are learned again");
    sn_say(SN_NOTE, "THE RADIO BOARD IS LEARNING BOTH BANDS AGAIN (ABOUT 40 S).", 40000);
}

static void jam_learn(void)
{
    bool hot = s_sp.st.sub_band == SENTINEL_BAND_UP || s_sp.st.nrf_band == SENTINEL_BAND_UP || s_sp.b.jam.level != SENTINEL_JAM_NONE;
    sn_confirm("TREAT WHAT IS ON THE AIR NOW AS NORMAL?",
               hot ? "A band is abnormal RIGHT NOW. If that is jamming, this teaches the board that jamming is normal, and it "
                     "stops warning about it.\n\nOnly do this if you know what is causing it and it belongs here."
                   : "The board forgets what it has learned about both bands and learns them again from what it hears in the "
                     "next 40 seconds. Do this after moving to a new place.",
               "RELEARN", jam_learn_yes);
}

static void jam_build(lv_obj_t *body)
{
    begin(jam_render);
    s_sp.state = sn_state(body);
    s_sp.v[0]  = sn_value(body);
    sn_heading(body, "THE EVIDENCE");
    for (int i = 1; i < 5; i++) s_sp.v[i] = sn_value(body);
    sn_action(HID_LETTER('R'), "R", "RELEARN WHAT IS NORMAL", jam_learn);
    sn_help("The jam watch runs all the time, whatever screen is open.\n\n"
            "POSSIBLE: a whole band has changed together. Half the 433 MHz band rising at once, or 40 or more of the 126 "
            "channels at 2.4 GHz solid, is not what ordinary traffic does.\n\n"
            "LIKELY: that, and something else went down with it: Bluetooth advertising nearly stopped, or the GPS lost "
            "satellites.\n\n"
            "It measures energy. It cannot tell a jammer from a very loud, broken or badly behaved transmitter.");
    jam_render();
}

/* ---- The menu ------------------------------------------------------------------ */

static void menu_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    bool s_up = st->sub_band == SENTINEL_BAND_UP, n_up = st->nrf_band == SENTINEL_BAND_UP;
    /* When something is wrong the tile says only that, so the red is about one thing. */
    if (s_up && n_up) snprintf(line, n, "433 MHZ BAND UP\n2.4 GHZ SATURATED");
    else if (s_up) snprintf(line, n, "433 MHZ: THE WHOLE BAND IS UP");
    else if (n_up) snprintf(line, n, "2.4 GHZ IS SATURATED");
    else snprintf(line, n, "433 MHZ NORMAL\n2.4 GHZ NORMAL");
    *level = s_up || n_up ? SN_ALERT : SN_NOTE;
}

static void jam_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    bool up = st->sub_band == SENTINEL_BAND_UP || st->nrf_band == SENTINEL_BAND_UP;
    snprintf(line, n, "%s", up ? "A WHOLE BAND IS ABNORMAL NOW" : "NO SIGN OF JAMMING");
    *level = up ? SN_ALERT : SN_NOTE;
}

static void sv_status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)st;
    (void)level;
    if (s_sv.on) snprintf(line, n, "MEASURING, %u S LEFT", (unsigned)((s_sv.end_ms - sentinel_now_ms() + 999) / 1000));
    else snprintf(line, n, "%s", s_sv.file[0] ? "LAST ONE SAVED" : "Measure this place for a minute and keep it as a file.");
}

static const sn_screen_t s_sub = {.name = "433 MHZ", .purpose = "The 433 MHz band now and over the last minute.", .quote = "Amaze! Amaze! Amaze!", /* Project Hail Mary */ .icon = ICON_ANTENNA, .flags = SN_BOARD, .build = sub_build, .destroy = end};
static const sn_screen_t s_tx  = {.name = "TRANSMITTERS", .purpose = "What transmits on one 433 MHz frequency, and how often.", .quote = "Party on, dudes!", /* Bill & Ted's Excellent Adventure */ .icon = ICON_REFRESH, .flags = SN_BOARD, .build = tx_build, .destroy = tx_destroy};
static const sn_screen_t s_nrf = {.name = "2.4 GHZ", .purpose = "The 2.4 GHz band now and over the last five minutes.", .quote = "We got both kinds. We got country and western.", /* The Blues Brothers */ .icon = ICON_GAUGE, .flags = SN_BOARD, .build = nrf_build, .destroy = end};
static const sn_screen_t s_site = {.name = "SITE SURVEY", .purpose = "Measure this place for a minute and keep it as a file.", .quote = "That rug really tied the room together.", /* The Big Lebowski */ .icon = ICON_PIN, .flags = SN_BOARD, .build = sv_build, .destroy = end, .status = sv_status};
static const sn_screen_t s_jam = {.name = "JAM WATCH", .purpose = "Is someone jamming, and what is the evidence?", .quote = "Strange things are afoot at the Circle K.", /* Bill & Ted's Excellent Adventure */ .icon = ICON_ALERT, .flags = SN_BOARD, .build = jam_build, .destroy = end, .status = jam_status};

static const sn_screen_t *const s_kids[] = {&s_sub, &s_tx, &s_nrf, &s_site, &s_jam};

const sn_screen_t sn_spectrum_menu = {
    .name    = "SPECTRUM",
    .purpose = "The radio bands themselves: 433 MHz and 2.4 GHz.", .quote = "I'm going to have to science the shit out of this.", /* The Martian */
    .icon    = ICON_GAUGE,
    .kids    = s_kids,
    .nkids   = 5,
    .status  = menu_status,
};
