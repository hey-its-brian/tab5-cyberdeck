/*
 * Fake-BIOS power-on self test. It reports real hardware state (keyboard,
 * SD, RTC, battery) so it doubles as a quick health check at power-up.
 * Any tap or key skips it.
 */
#include "deck_boot.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define LINE_MS 120
#define HOLD_MS 650
#define MAX_LINES 12

typedef enum { ST_OK, ST_WARN, ST_DEFER, ST_NONE } status_t;

typedef struct {
    char text[64];
    status_t st;
    char note[24];
} post_line_t;

static lv_obj_t *s_overlay;
static lv_obj_t *s_list;
static lv_timer_t *s_timer;
static post_line_t s_lines[MAX_LINES];
static int s_count;
static int s_shown;
static uint32_t s_done_at;
static bool s_finishing;

static void add(status_t st, const char *note, const char *fmt, ...)
{
    if (s_count >= MAX_LINES) return;
    post_line_t *l = &s_lines[s_count++];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->text, sizeof(l->text), fmt, ap);
    va_end(ap);
    l->st = st;
    snprintf(l->note, sizeof(l->note), "%s", note ? note : "");
}

static void collect(void)
{
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    s_count = 0;
    add(ST_OK, NULL, "CPU  %s @ %luMHz", si.chip, (unsigned long)si.cpu_mhz);
    add(ST_OK, NULL, "RAM  %luK PSRAM / %luK SRAM", (unsigned long)(si.psram_total / 1024),
        (unsigned long)(si.heap_int_total / 1024));
    add(ST_OK, NULL, "VID  %s 1280x720 PPA-ROT", si.board);
    if (hal_kbd_present()) {
        add(ST_OK, NULL, "I/O  TAB5 KEYBOARD FW 0x%02X", hal_kbd_fw_version());
    } else {
        add(ST_WARN, "NOT DETECTED", "I/O  TAB5 KEYBOARD");
    }
    add(hal_rtc_present() ? ST_OK : ST_WARN, hal_rtc_present() ? NULL : "MISSING", "RTC  RX8130");
    if (hal_sd_mounted()) {
        add(ST_OK, NULL, "STO  SDCARD %.1fG", (double)hal_sd_total_bytes() / 1e9);
    } else {
        add(ST_WARN, "NO MEDIA", "STO  SDCARD");
    }
    /* The C6 link is usually still coming up this early; say what we know. */
    net_state_t ns = net_state();
    if (ns == NET_NO_RADIO) {
        add(ST_WARN, "NO RADIO", "NET  ESP32-C6 WIFI6");
    } else if (ns == NET_STARTING) {
        add(ST_DEFER, "LINKING", "NET  ESP32-C6 WIFI6");
    } else {
        add(ST_OK, NULL, "NET  ESP32-C6 WIFI6");
    }
    hal_power_t p;
    hal_power_read(&p);
    if (p.valid && p.percent >= 0) {
        add(ST_OK, NULL, "PWR  %.2fV  %d%%%s", (double)p.volts, p.percent, p.charging ? " CHG" : "");
    } else if (p.valid) {
        add(ST_OK, NULL, "PWR  USB %.2fV", (double)p.volts);
    } else {
        add(ST_WARN, "NO SENSOR", "PWR  INA226");
    }
    if (ota_pending_verify()) {
        add(ST_WARN, "SELF-TEST", "FW   DECK//OS v%s UPDATED", DECK_VERSION);
    } else {
        add(ST_OK, NULL, "FW   DECK//OS v%s", DECK_VERSION);
    }
    add(ST_NONE, NULL, "> JACKING IN");
}

static void show_line(const post_line_t *l)
{
    lv_obj_t *row = deck_box(s_list);
    lv_obj_set_width(row, 760);

    lv_obj_t *t = deck_label(row, g_font.mono_m, g_pal.text, l->text);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    if (l->st == ST_NONE) {
        lv_obj_set_style_text_color(t, g_pal.accent, 0);
        deck_fx_typewriter(t, l->text, 30);
        return;
    }

    const char *tag   = "[  OK  ]";
    lv_color_t color  = g_pal.ok;
    if (l->st == ST_WARN) {
        tag   = "[ WARN ]";
        color = g_pal.warn;
    } else if (l->st == ST_DEFER) {
        tag   = "[ DEFR ]";
        color = g_pal.dim;
    }
    char buf[48];
    if (l->note[0]) {
        snprintf(buf, sizeof(buf), "%s %s", l->note, tag);
    } else {
        snprintf(buf, sizeof(buf), "%s", tag);
    }
    lv_obj_t *s = deck_label(row, g_font.mono_m, color, buf);
    lv_obj_align(s, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void finish_mid(void *u)
{
    (void)u;
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
    }
}

static void finish(void)
{
    if (s_finishing) return;
    s_finishing = true;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    deck_fx_glitch(320, finish_mid, NULL, NULL);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (s_shown < s_count) {
        show_line(&s_lines[s_shown++]);
        if (s_shown == s_count) s_done_at = lv_tick_get();
        return;
    }
    if (lv_tick_elaps(s_done_at) >= HOLD_MS) {
        finish();
    }
}

static void overlay_clicked(lv_event_t *e)
{
    (void)e;
    deck_boot_skip();
}

void deck_boot_run(void)
{
    collect();
    s_shown     = 0;
    s_finishing = false;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_move_background(s_overlay); /* keep the scanlines on top of it */

    lv_obj_t *col = deck_box(s_overlay);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_align(col, LV_ALIGN_TOP_LEFT, 80, 60);

    lv_obj_t *brand = deck_box(col);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(brand, 4, 0);
    deck_label(brand, g_font.disp_xl, g_pal.text, "DECK");
    deck_label(brand, g_font.disp_xl, g_pal.accent, "//OS");
    lv_obj_t *ver = deck_label(brand, g_font.mono_m, g_pal.dim, "  BIOS v" DECK_VERSION);
    lv_obj_set_style_pad_bottom(ver, 10, 0);

    lv_obj_t *sub = deck_label(col, g_font.mono_m, g_pal.accent2, "POWER-ON SELF TEST // M5STACK TAB5");
    lv_obj_set_style_pad_bottom(sub, 18, 0);

    s_list = deck_box(col);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);

    lv_obj_t *hint = deck_label(s_overlay, g_font.mono_s, g_pal.dim, "TAP OR PRESS ANY KEY TO SKIP");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -40, -30);

    s_timer = lv_timer_create(tick, LINE_MS, NULL);
}

void deck_boot_skip(void)
{
    if (s_overlay != NULL) finish();
}

bool deck_boot_active(void) { return s_overlay != NULL; }
