/*
 * SENTINEL > WARDRIVE: record the Wi-Fi access points and Bluetooth devices
 * heard, each with the GPS position, to the SD card in WiGLE's format.
 * Listening only: nothing is transmitted.
 *
 * One screen, one job: start a drive, see that it is recording, end it.
 * The drive keeps running with the screen asleep. It ends when this screen
 * is left, because every other screen wants the board's radio for something
 * else; leaving while a drive runs asks first.
 */
#include "sn_ui.h"

#include <stdio.h>
#include <string.h>

#include "deck_attr.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_shell.h"
#include "deck_theme.h"

#define TICK_MS       500
#define OPEN_GRACE_MS 2000 /* how long a drive may run with its files not yet open before that is an error */

static struct {
    lv_obj_t   *state, *why, *wifi, *ble, *v[4];
    lv_timer_t *timer;
    bool        leaving;
    sentinel_state_t      st;
    sentinel_drive_info_t info;
} s_drv DECK_PSRAM_BSS;

static uint32_t s_ran_ms; /* how long the last drive ran: outlives the screen, like its counts */

static void toggle(void);

/* Why a drive cannot start, or NULL when it can. */
static const char *blocked(const sentinel_state_t *st)
{
    if (!st->hello) return "The radio board has not introduced itself yet.";
    if (!st->drv_ok) return "The board's firmware has no drive tool (needs 0.6.0).";
    if (!hal_storage_is_sd()) return "A drive needs an SD card.";
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

    /* The files are opened by the writer task a moment after the drive begins. */
    bool opening = in->on && !in->writing && sentinel_now_ms() - in->started_ms < OPEN_GRACE_MS;
    if (opening) {
        sn_set(s_drv.state, g_pal.label, "STARTING");
        sn_set(s_drv.why, g_pal.label, "Opening the files on the SD card.");
    } else if (in->on && !in->writing) {
        sn_set(s_drv.state, g_pal.danger, "!! NOT WRITING");
        sn_set(s_drv.why, g_pal.text, "The files could not be opened on the SD card. End the drive and check the card.");
    } else if (in->on && !sentinel_link(st)) {
        sn_set(s_drv.state, g_pal.warn, "! NO LINK TO THE BOARD");
        sn_set(s_drv.why, g_pal.text, "The drive picks up again when the board answers.");
    } else if (in->on && !in->placed) {
        sn_set(s_drv.state, g_pal.warn, "! WAITING FOR A GPS FIX");
        sn_set(s_drv.why, g_pal.text, "Nothing is written until the GPS gives a position and a date.");
    } else if (in->on) {
        sn_set(s_drv.state, g_pal.ok, "RECORDING");
        sn_set(s_drv.why, g_pal.label, "Listening on every Wi-Fi channel and to Bluetooth. Nothing is transmitted. The screen can sleep.");
    } else if (block) {
        sn_set(s_drv.state, g_pal.warn, "! CANNOT START");
        sn_set(s_drv.why, g_pal.text, "%s", block);
    } else if (in->rows || in->survey_rows) {
        sn_set(s_drv.state, g_pal.text, "ENDED");
        sn_set(s_drv.why, g_pal.label, "The file is on the SD card. Copy it with FILES or over the portal.");
    } else {
        sn_set(s_drv.state, g_pal.text, "READY");
        sn_set(s_drv.why, g_pal.label, placed ? "Press Enter to start a drive." : "Press Enter to start. There is no GPS fix yet: the drive will wait for one.");
    }

    uint32_t ran = in->on ? sentinel_now_ms() - in->started_ms : s_ran_ms;
    if (in->on) s_ran_ms = ran;
    sn_set(s_drv.wifi, lv_color_white(), "%u", (unsigned)in->wifi);
    sn_set(s_drv.ble, lv_color_white(), "%u", (unsigned)in->ble);
    if (in->distance_m >= 1000.0f)
        sn_set(s_drv.v[0], g_pal.text, "TIME %02u:%02u:%02u     DISTANCE %.2f KM", (unsigned)(ran / 3600000), (unsigned)(ran / 60000 % 60), (unsigned)(ran / 1000 % 60),
               (double)in->distance_m / 1000.0);
    else
        sn_set(s_drv.v[0], g_pal.text, "TIME %02u:%02u:%02u     DISTANCE %u M", (unsigned)(ran / 3600000), (unsigned)(ran / 60000 % 60), (unsigned)(ran / 1000 % 60),
               (unsigned)in->distance_m);
    if (placed) sn_set(s_drv.v[1], g_pal.ok, "GPS FIX: %u SATELLITES, GOOD TO %.0f M", (unsigned)st->sats_used, (double)w.acc_m);
    else
        sn_set(s_drv.v[1], g_pal.warn, "! %s (%u SATELLITES USED, %u IN VIEW)",
               !st->gps_ok ? "THE GPS IS SILENT" : !st->fix ? "NO GPS FIX" : st->hdop > SENTINEL_DRIVE_HDOP_MAX ? "GPS FIX TOO POOR" : "NO DATE FROM THE GPS YET", (unsigned)st->sats_used,
               (unsigned)st->sats_view);
    bool has_file = in->file[0] && (in->on ? in->writing : (in->rows || in->survey_rows));
    sn_set(s_drv.v[2], has_file ? g_pal.text : g_pal.label, "FILE: %s", has_file ? in->file : "NONE YET");
    if (in->lost) sn_set(s_drv.v[3], g_pal.danger, "!! %u ROWS DID NOT REACH THE CARD", (unsigned)in->lost);
    else if (in->rows) sn_set(s_drv.v[3], g_pal.label, "%u rows written, none lost.", (unsigned)in->rows);
    else sn_set(s_drv.v[3], g_pal.label, " ");

    sn_action_label(toggle, in->on ? "END THE DRIVE" : "START A DRIVE");
    sn_action_danger(toggle, in->on);
    sn_action_enable(toggle, in->on || !block);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    render();
}

static void end_yes(void)
{
    sentinel_drive_stop();
    if (s_drv.leaving) {
        s_drv.leaving = false;
        sn_back();
        return;
    }
    render();
}

static void ask_end(bool leaving)
{
    static char msg[160];
    sentinel_drive_info(&s_drv.info);
    snprintf(msg, sizeof(msg), "%s%02u:%02u:%02u so far, %u rows written. The file is kept.", leaving ? "Leaving this screen ends the drive.\n\n" : "",
             (unsigned)(s_ran_ms / 3600000), (unsigned)(s_ran_ms / 60000 % 60), (unsigned)(s_ran_ms / 1000 % 60), (unsigned)s_drv.info.rows);
    s_drv.leaving = leaving;
    sn_confirm("END THE DRIVE?", msg, "END IT", end_yes);
}

static void toggle(void)
{
    sentinel_drive_info(&s_drv.info);
    if (s_drv.info.on) {
        ask_end(false);
    } else {
        sentinel_snapshot(&s_drv.st);
        if (!blocked(&s_drv.st)) sentinel_drive_start(DECK_VERSION);
        render();
    }
}

static lv_obj_t *count(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *col = sn_col(parent, 300);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_t *n = lv_label_create(col);
    lv_obj_set_style_text_font(n, g_font.disp_xl, 0);
    lv_label_set_text(n, "0");
    lv_obj_t *c = lv_label_create(col);
    lv_obj_set_style_text_font(c, g_font.mono_b, 0);
    lv_obj_set_style_text_color(c, g_pal.label, 0);
    lv_label_set_text(c, caption);
    return n;
}

static void build(lv_obj_t *body)
{
    memset(&s_drv, 0, sizeof(s_drv));
    s_drv.state = sn_state(body);
    s_drv.why   = sn_note(body);
    lv_obj_set_style_text_font(s_drv.why, g_font.mono_b, 0);
    lv_obj_t *big = sn_row_box(body);
    lv_obj_set_style_margin_top(big, 10, 0);
    s_drv.wifi = count(big, "WI-FI ACCESS POINTS");
    s_drv.ble  = count(big, "BLUETOOTH DEVICES");
    for (int i = 0; i < 3; i++) s_drv.v[i] = sn_value(body);
    s_drv.v[3] = sn_note(body);
    sn_action(HID_ENTER, "ENTER", "START A DRIVE", toggle);
    sn_help("A drive writes one row for each access point and Bluetooth device heard, with where and when, in the format "
            "wigle.net reads. It also keeps a second file of the 433 MHz and 2.4 GHz bands along the way.\n\n"
            "One row per device per place, and another after you have moved 25 m. Never written: networks that opt out of "
            "mapping (names ending _nomap or _optout), and Bluetooth addresses that change over time (phones, watches).\n\n"
            "A row needs a place and a time, both from the GPS, so nothing is written until there is a fix.\n\n"
            "The drive ends when you leave this screen; you will be asked first.");
    s_drv.timer = lv_timer_create(tick, TICK_MS, NULL);
    render();
}

static void destroy(void)
{
    if (s_drv.timer) lv_timer_delete(s_drv.timer);
    s_drv.timer = NULL;
    sentinel_drive_stop(); /* the drive ends with its screen: another screen wants the radio */
}

/* BACK while a drive runs: ask before ending it. */
static bool back(void)
{
    sentinel_drive_info(&s_drv.info);
    if (!s_drv.info.on) return false;
    ask_end(true);
    return true;
}

static void status(const sentinel_state_t *st, char *line, size_t n, sn_level_t *level)
{
    (void)level;
    snprintf(line, n, "%s\n%s", st->fix ? "GPS FIX" : "NO GPS FIX YET", hal_storage_is_sd() ? "SD CARD READY" : "NO SD CARD");
    if (!st->fix || !hal_storage_is_sd()) *level = SN_NOTE;
}

const sn_screen_t sn_drive_screen = {
    .name    = "WARDRIVE",
    .purpose = "Record what is heard, with its position, as you move.",
    .icon    = ICON_PIN,
    .flags   = SN_BOARD,
    .build   = build,
    .destroy = destroy,
    .back    = back,
    .status  = status,
};
