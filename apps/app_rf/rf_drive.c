/*
 * RF WATCH > WARDRIVE: carry the deck and it writes down what it hears and
 * where. Enter starts a drive, Enter again (or leaving this screen) ends it.
 *
 * While a drive runs the C5 hops every Wi-Fi channel with its Bluetooth
 * scan beside it, all of it listening: no probe requests, no scan requests.
 * That is why this works in a SAFE session. What it hears goes to
 * sentinel/wigle-<time>.csv on the SD card in WiGLE's format, and the two
 * swept bands go to sentinel/survey-<time>.csv along the same route.
 *
 * What is written and what is not (no fix, a network that opted out, a
 * Bluetooth address that rotates) is decided in sentinel_drive.h; this
 * screen shows the counts so the user can see each rule at work.
 *
 * The drive is the driver's, not this screen's (deck_sentinel.h): it keeps
 * running while the screen sleeps, which on a long drive it should.
 */
#include "rf_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define TICK_MS       500
#define OPEN_GRACE_MS 2000 /* how long a drive may run with its files not yet open before that is an error */

static struct {
    lv_obj_t *state, *why, *file, *span;
    lv_obj_t *wifi, *ble, *rows, *skipped, *lost;
    lv_obj_t *fix, *pos, *when, *node;
    lv_obj_t *row[SENTINEL_DRIVE_RECENT];
    lv_timer_t *timer;
    uint32_t  ran_ms; /* how long the drive that just ended ran */
    sentinel_state_t      st;
    sentinel_drive_info_t info;
} s_drv DECK_PSRAM_BSS;

/* Why a drive cannot start, or NULL when it can. */
static const char *blocked(const sentinel_state_t *st)
{
    if (!st->powered) return "THE BOARD IS OFF";
    if (!sentinel_link(st) || !st->hello) return "NO LINK TO THE NODE";
    if (!st->drv_ok) return "THE NODE'S FIRMWARE HAS NO DRIVE TOOL (NEEDS 0.6.0)";
    if (!hal_storage_is_sd()) return "A DRIVE NEEDS AN SD CARD";
    return NULL;
}

static void render(void)
{
    sentinel_snapshot(&s_drv.st);
    sentinel_drive_info(&s_drv.info);
    const sentinel_state_t      *st = &s_drv.st;
    const sentinel_drive_info_t *in = &s_drv.info;
    sentinel_where_t w;
    bool        placed = sentinel_where(&w, st, sentinel_now_ms());
    const char *block  = blocked(st);

    const char *state, *why;
    lv_color_t  c;
    /* The files are opened by the writer task a moment after the drive begins. */
    bool opening = in->on && !in->writing && sentinel_now_ms() - in->started_ms < OPEN_GRACE_MS;
    if (opening) {
        state = "STARTING", c = g_pal.dim;
        why   = "OPENING THE FILES ON THE SD CARD.";
    } else if (in->on && !in->writing) {
        state = "NOT WRITING", c = g_pal.danger;
        why   = "THE FILES COULD NOT BE OPENED ON THE SD CARD. END THE DRIVE AND CHECK THE CARD.";
    } else if (in->on && !sentinel_link(st)) {
        state = "NO LINK", c = g_pal.warn;
        why   = "THE NODE STOPPED ANSWERING. THE DRIVE RESUMES WHEN IT COMES BACK.";
    } else if (in->on && !in->placed) {
        state = "WAITING FOR A FIX", c = g_pal.warn;
        why   = "SIGHTINGS ARE COUNTED BUT NOT WRITTEN UNTIL THE GPS GIVES A POSITION AND A DATE.";
    } else if (in->on) {
        state = "RECORDING", c = g_pal.ok;
        why   = strcmp(st->mode, "drive") == 0 ? "WI-FI ON EVERY CHANNEL, BLUETOOTH BESIDE IT. NOTHING TRANSMITS."
                                               : "ASKING THE NODE FOR THE DRIVE TOOL. UNTIL IT ANSWERS, ONLY ITS BACKGROUND SCAN IS HEARD.";
    } else if (block) {
        state = "CANNOT START", c = g_pal.warn;
        why   = block;
    } else if (in->rows || in->survey_rows) {
        state = "ENDED", c = g_pal.text;
        why   = "THE FILE IS ON THE SD CARD: COPY IT WITH FILES OR OVER THE PORTAL. [ENTER] STARTS ANOTHER.";
    } else {
        state = "READY", c = g_pal.dim;
        why   = placed ? "[ENTER] STARTS A DRIVE." : "[ENTER] STARTS A DRIVE. THERE IS NO FIX YET: IT WILL WAIT FOR ONE.";
    }
    lv_label_set_text(s_drv.state, state);
    lv_obj_set_style_text_color(s_drv.state, c, 0);
    lv_label_set_text(s_drv.why, why);
    bool has_file = in->file[0] && (in->on ? in->writing : (in->rows || in->survey_rows));
    lv_label_set_text_fmt(s_drv.file, "%s", has_file ? in->file : "NO FILE YET");

    uint32_t ran = in->on ? sentinel_now_ms() - in->started_ms : s_drv.ran_ms;
    if (in->on) s_drv.ran_ms = ran;
    if (in->distance_m >= 1000.0f)
        lv_label_set_text_fmt(s_drv.span, "%02u:%02u:%02u    %.2f KM", (unsigned)(ran / 3600000), (unsigned)(ran / 60000 % 60),
                              (unsigned)(ran / 1000 % 60), (double)in->distance_m / 1000.0);
    else
        lv_label_set_text_fmt(s_drv.span, "%02u:%02u:%02u    %u M", (unsigned)(ran / 3600000), (unsigned)(ran / 60000 % 60),
                              (unsigned)(ran / 1000 % 60), (unsigned)in->distance_m);

    lv_label_set_text_fmt(s_drv.wifi, "%u", (unsigned)in->wifi);
    lv_label_set_text_fmt(s_drv.ble, "%u", (unsigned)in->ble);
    lv_label_set_text_fmt(s_drv.rows, "ROWS %u    SURVEY ROWS %u", (unsigned)in->rows, (unsigned)in->survey_rows);
    lv_label_set_text_fmt(s_drv.skipped, "NOT WRITTEN:  NO FIX %u    OPTED OUT %u    ROTATING ADDRESS %u", (unsigned)in->nofix,
                          (unsigned)in->optout, (unsigned)in->priv);
    lv_label_set_text_fmt(s_drv.lost, in->lost ? "%u ROWS DID NOT REACH THE CARD" : "NOTHING LOST ON THE WAY TO THE CARD",
                          (unsigned)in->lost);
    lv_obj_set_style_text_color(s_drv.lost, in->lost ? g_pal.danger : g_pal.dim, 0);

    if (placed) {
        lv_label_set_text_fmt(s_drv.fix, "FIX   %u SATS   HDOP %.1f   +/- %.0f M", (unsigned)st->sats_used, (double)st->hdop,
                              (double)w.acc_m);
        lv_obj_set_style_text_color(s_drv.fix, g_pal.ok, 0);
        lv_label_set_text_fmt(s_drv.pos, "%.6f  %.6f   ALT %.0f M", w.lat, w.lon, (double)w.alt_m);
        lv_label_set_text_fmt(s_drv.when, "%04u-%02u-%02u %02u:%02u:%02u UTC  (FROM THE GPS)", (unsigned)w.year, (unsigned)w.mon,
                              (unsigned)w.day, (unsigned)w.hour, (unsigned)w.min, (unsigned)w.sec);
    } else {
        const char *no = !st->gps_ok ? "THE GPS IS SILENT" : !st->fix ? "NO FIX" : st->hdop > SENTINEL_DRIVE_HDOP_MAX ? "FIX TOO POOR"
                                                                                                                      : "NO DATE FROM THE GPS YET";
        lv_label_set_text_fmt(s_drv.fix, "%s   %u SATS USED, %u IN VIEW   HDOP %.1f", no, (unsigned)st->sats_used,
                              (unsigned)st->sats_view, (double)st->hdop);
        lv_obj_set_style_text_color(s_drv.fix, g_pal.warn, 0);
        lv_label_set_text(s_drv.pos, "---.------  ---.------");
        lv_label_set_text(s_drv.when, "A ROW NEEDS A PLACE AND A TIME, BOTH FROM THE GPS");
    }
    lv_label_set_text_fmt(s_drv.node, "NODE TOOL %s    433 MHZ SWEEPS %u    2.4 GHZ SWEEPS %u", st->mode, (unsigned)st->sub_sweeps,
                          (unsigned)st->nrf_sweeps);

    for (int r = 0; r < SENTINEL_DRIVE_RECENT; r++) {
        if (r < in->recent_n) {
            lv_label_set_text_fmt(s_drv.row[r], "%4d  %s", (int)in->recent[r].rssi, in->recent[r].text);
            lv_obj_set_style_text_color(s_drv.row[r], in->recent[r].ble ? g_pal.accent : g_pal.text, 0);
        } else {
            lv_label_set_text(s_drv.row[r], r == 0 ? "NOTHING YET" : "");
            lv_obj_set_style_text_color(s_drv.row[r], g_pal.dim, 0);
        }
    }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    render();
}

static lv_obj_t *wrapped(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = deck_label(parent, font, color, "");
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

/* A big number with its caption under it. */
static lv_obj_t *count(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *col = deck_box(parent);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *n = deck_label(col, g_font.disp_l, g_pal.text, "0");
    deck_label(col, g_font.mono_s, g_pal.dim, caption);
    return n;
}

void rf_drive_build(lv_obj_t *parent)
{
    memset(&s_drv, 0, sizeof(s_drv));
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
    lv_obj_set_size(left, 620, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 10, 0);

    lv_obj_t *p = rf_panel(left, "WARDRIVE  RECORD ONLY");
    s_drv.state = deck_label(p, g_font.disp_l, g_pal.dim, "");
    s_drv.why   = wrapped(p, g_font.mono_s, g_pal.dim);
    s_drv.file  = deck_label(p, g_font.mono_m, g_pal.text, "");
    s_drv.span  = deck_label(p, g_font.mono_m, g_pal.text, "");

    p = rf_panel(left, "ON FILE");
    lv_obj_t *big = deck_box(p);
    lv_obj_set_width(big, LV_PCT(100));
    lv_obj_set_flex_flow(big, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(big, 60, 0);
    s_drv.wifi    = count(big, "WI-FI ACCESS POINTS");
    s_drv.ble     = count(big, "BLUETOOTH DEVICES");
    s_drv.rows    = deck_label(p, g_font.mono_m, g_pal.text, "");
    s_drv.skipped = wrapped(p, g_font.mono_s, g_pal.dim);
    s_drv.lost    = deck_label(p, g_font.mono_s, g_pal.dim, "");

    p = rf_panel(left, "WHERE AND WHEN");
    lv_obj_set_flex_grow(p, 1);
    s_drv.fix  = deck_label(p, g_font.mono_m, g_pal.dim, "");
    s_drv.pos  = deck_label(p, g_font.mono_m, g_pal.text, "");
    s_drv.when = deck_label(p, g_font.mono_s, g_pal.dim, "");
    s_drv.node = deck_label(p, g_font.mono_s, g_pal.dim, "");

    lv_obj_t *right = deck_box(cols);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    p = rf_panel(right, "LAST WRITTEN   dBm  WHAT");
    lv_obj_set_flex_grow(p, 1);
    lv_obj_set_style_pad_row(p, 2, 0);
    for (int r = 0; r < SENTINEL_DRIVE_RECENT; r++) {
        lv_obj_t *row = deck_label(p, g_font.mono_m, g_pal.dim, "");
        lv_obj_set_width(row, LV_PCT(100));
        lv_label_set_long_mode(row, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(row, lv_font_get_line_height(g_font.mono_m) + 8);
        lv_obj_set_style_pad_ver(row, 4, 0);
        s_drv.row[r] = row;
    }
    lv_obj_t *rules = wrapped(p, g_font.mono_s, g_pal.dim);
    lv_label_set_text(rules, "ONE ROW PER DEVICE PER PLACE, AND AGAIN AFTER 25 M.\nNEVER WRITTEN: NETWORKS THAT OPT OUT OF MAPPING, "
                             "AND BLUETOOTH ADDRESSES THAT ROTATE (PHONES, WATCHES).");

    deck_label(root, g_font.mono_s, g_pal.dim,
               "[ENTER] START OR END THE DRIVE   [ESC] TOOLS (ENDS THE DRIVE)      THE SCREEN CAN SLEEP: THE DRIVE GOES ON");

    s_drv.timer = lv_timer_create(tick, TICK_MS, NULL);
    render();
}

void rf_drive_destroy(void)
{
    if (s_drv.timer) lv_timer_delete(s_drv.timer);
    s_drv.timer = NULL;
    sentinel_drive_stop(); /* the drive ends with its screen: another tool wants the radio */
    memset(s_drv.row, 0, sizeof(s_drv.row));
}

bool rf_drive_key(const deck_key_t *k)
{
    if (!s_drv.timer || (k->mods & (DECK_MOD_CTRL | DECK_MOD_ALT))) return false;
    if (k->code != HID_ENTER && k->code != HID_SPACE) return false;
    sentinel_drive_info(&s_drv.info);
    if (s_drv.info.on) {
        sentinel_drive_stop();
    } else {
        sentinel_snapshot(&s_drv.st);
        if (!blocked(&s_drv.st)) sentinel_drive_start(DECK_VERSION);
    }
    render();
    return true;
}
