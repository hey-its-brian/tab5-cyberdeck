/*
 * SYSTEM module, three columns:
 *   DISPLAY   brightness, scanlines, boot POST, accent
 *   NETWORK   Wi-Fi status and setup, timezone, clock (manual or NTP)
 *   SYSTEM    live hardware readout and firmware updates
 */
#include "app_sys.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

typedef struct {
    lv_obj_t *bright_val;
    lv_obj_t *clock_val;
    lv_obj_t *info[10];
    lv_obj_t *modal;
    lv_obj_t *roll[5];
    lv_timer_t *timer;

    lv_obj_t *net_state;
    lv_obj_t *net_ssid;
    lv_obj_t *net_ip;
    lv_obj_t *net_signal;
    lv_obj_t *join_btn;
    lv_obj_t *tz_val;
    lv_obj_t *ntp;
    lv_timer_t *scan_poll;
    lv_obj_t *ota_status;
    lv_obj_t *ota_btn;
} sys_ui_t;

static sys_ui_t s_ui;

enum { INFO_BOARD, INFO_CHIP, INFO_PSRAM, INFO_SRAM, INFO_UPTIME, INFO_KBD, INFO_SD, INFO_PWR, INFO_FW, INFO_COUNT };

static const char *s_info_keys[INFO_COUNT] = {
    "BOARD", "CHIP", "PSRAM", "SRAM", "UPTIME", "KEYBOARD", "STORAGE", "POWER", "FIRMWARE",
};

/* ---- Layout helpers ------------------------------------------------------ */

static lv_obj_t *column_panel(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *p = deck_panel(parent, DECK_CUT_TL | DECK_CUT_BR, 22);
    lv_obj_set_size(p, w, LV_PCT(100));
    deck_panel_set_tab(p, true);
    lv_obj_set_style_pad_all(p, 26, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 14, 0);
    return p;
}

/* A full-width row with a label on the left; returns the row so the caller
 * can add a control aligned right. */
static lv_obj_t *setting_row(lv_obj_t *parent, const char *icon, const char *name)
{
    lv_obj_t *row = deck_box(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 48);
    lv_obj_t *l = deck_icon_text(row, icon, name, g_font.mono_m, g_pal.text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

/* ---- Display settings ---------------------------------------------------- */

static void bright_changed(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    int32_t v   = lv_slider_get_value(s);
    if (s_ui.bright_val == NULL) return;
    hal_backlight_set((uint8_t)v);
    lv_label_set_text_fmt(s_ui.bright_val, "%ld%%", (long)v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_DEFOCUSED) {
        hal_cfg_set_i32("bright", v);
    }
}

static void accent_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx == deck_theme_accent()) return;
    hal_cfg_set_i32("accent", idx);
    deck_theme_set_accent(idx);
    deck_shell_rebuild();
}

static void scan_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("scan", on);
    deck_fx_scanlines(on);
}

static void boot_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("boot", on);
}

static lv_obj_t *add_switch(lv_obj_t *row, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 76, 38);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* ---- Clock modal --------------------------------------------------------- */

/* The modal took over the focus group, so closing it rebuilds the module,
 * which restores the normal focus order (and deletes the modal). */
static void modal_close(void)
{
    if (s_ui.modal) {
        s_ui.modal = NULL;
        deck_shell_rebuild();
    }
}

static void modal_cancel(lv_event_t *e)
{
    (void)e;
    modal_close();
}

static void modal_set(lv_event_t *e)
{
    (void)e;
    struct tm t = {0};
    t.tm_year   = 2024 + (int)lv_roller_get_selected(s_ui.roll[0]) - 1900;
    t.tm_mon    = (int)lv_roller_get_selected(s_ui.roll[1]);
    t.tm_mday   = 1 + (int)lv_roller_get_selected(s_ui.roll[2]);
    t.tm_hour   = (int)lv_roller_get_selected(s_ui.roll[3]);
    t.tm_min    = (int)lv_roller_get_selected(s_ui.roll[4]);
    t.tm_isdst  = -1;
    hal_rtc_set(&t);
    modal_close();
}

static lv_obj_t *make_roller(lv_obj_t *parent, int first, int count, int width_digits, int selected,
                             const char *caption)
{
    static char opts[31 * 5 + 64 * 3];
    size_t n = 0;
    opts[0]  = '\0';
    for (int i = 0; i < count; i++) {
        n += (size_t)snprintf(opts + n, sizeof(opts) - n, "%s%0*d", i ? "\n" : "", width_digits, first + i);
    }

    lv_obj_t *col = deck_box(parent);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, 0);
    deck_label(col, g_font.mono_s, g_pal.dim, caption);

    lv_obj_t *r = lv_roller_create(col);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, width_digits >= 4 ? 130 : 96);
    lv_obj_set_style_text_font(r, g_font.mono_l, 0);
    lv_obj_set_style_bg_color(r, g_pal.panel, 0);
    lv_obj_set_style_border_color(r, g_pal.line, 0);
    lv_obj_set_style_text_color(r, g_pal.dim, 0);
    lv_obj_set_style_bg_color(r, g_pal.accent, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, g_pal.bg, LV_PART_SELECTED);
    if (selected >= 0 && selected < count) lv_roller_set_selected(r, (uint32_t)selected, LV_ANIM_OFF);
    return r;
}

static void open_clock_modal(lv_event_t *e)
{
    lv_obj_t *app_root = (lv_obj_t *)lv_event_get_user_data(e);
    if (s_ui.modal) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    s_ui.modal = lv_obj_create(app_root);
    lv_obj_remove_style_all(s_ui.modal);
    lv_obj_add_flag(s_ui.modal, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ui.modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ui.modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.modal, LV_OPA_70, 0);

    lv_obj_t *p = deck_panel(s_ui.modal, DECK_CUT_TL | DECK_CUT_BR, 22);
    deck_panel_set_tab(p, true);
    deck_panel_set_outline(p, g_pal.accent);
    lv_obj_set_size(p, 760, LV_SIZE_CONTENT);
    lv_obj_center(p);
    lv_obj_set_style_pad_all(p, 30, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 20, 0);

    deck_section(p, "SET CLOCK");

    /* Rebuild the focus order for the modal only. */
    lv_group_t *g = deck_input_group();
    lv_group_remove_all_objs(g);

    lv_obj_t *row = deck_box(p);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    int year     = tm.tm_year + 1900;
    s_ui.roll[0] = make_roller(row, 2024, 17, 4, year - 2024, "YEAR");
    s_ui.roll[1] = make_roller(row, 1, 12, 2, tm.tm_mon, "MON");
    s_ui.roll[2] = make_roller(row, 1, 31, 2, tm.tm_mday - 1, "DAY");
    s_ui.roll[3] = make_roller(row, 0, 24, 2, tm.tm_hour, "HOUR");
    s_ui.roll[4] = make_roller(row, 0, 60, 2, tm.tm_min, "MIN");

    lv_obj_t *btns = deck_box(p);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 16, 0);
    lv_obj_t *cancel = deck_button(btns, "CANCEL", NULL);
    lv_obj_add_event_cb(cancel, modal_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *set = deck_button(btns, "SET", NULL);
    lv_obj_add_event_cb(set, modal_set, LV_EVENT_CLICKED, NULL);

    lv_group_focus_obj(s_ui.roll[3]);
}

/* ---- Network ------------------------------------------------------------- */

#define SCAN_SHOW 16

static net_ap_t s_aps[SCAN_SHOW];
static int s_ap_count;
static char s_join_ssid[33];

static void password_entered(const char *text, void *ud)
{
    (void)ud;
    if (text) net_connect(s_join_ssid, text);
}

static void other_ssid_entered(const char *text, void *ud)
{
    (void)ud;
    if (text == NULL || text[0] == '\0') return;
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", text);
    deck_modal_password(s_join_ssid, password_entered, NULL);
}

static void network_picked(int index, void *ud)
{
    (void)ud;
    if (index < 0) return;
    if (index == s_ap_count) { /* "Other network..." */
        deck_modal_prompt("HIDDEN NETWORK", "", other_ssid_entered, NULL);
        return;
    }
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", s_aps[index].ssid);
    if (s_aps[index].secure) {
        deck_modal_password(s_join_ssid, password_entered, NULL);
    } else {
        net_connect(s_join_ssid, "");
    }
}

static void scan_poll(lv_timer_t *t)
{
    if (!net_scan_done()) return;
    lv_timer_delete(t);
    s_ui.scan_poll = NULL;
    if (s_ui.join_btn) lv_label_set_text(lv_obj_get_child(s_ui.join_btn, 0), "JOIN");

    s_ap_count = (int)net_scan_results(s_aps, SCAN_SHOW);
    static char rows[SCAN_SHOW + 1][64];
    const char *items[SCAN_SHOW + 1];
    for (int i = 0; i < s_ap_count; i++) {
        int bars = s_aps[i].rssi > -55 ? 4 : s_aps[i].rssi > -65 ? 3 : s_aps[i].rssi > -75 ? 2 : 1;
        snprintf(rows[i], sizeof(rows[i]), "%-32.32s %s %.*s", s_aps[i].ssid, s_aps[i].secure ? "LOCK" : "OPEN",
                 bars, "||||");
        items[i] = rows[i];
    }
    snprintf(rows[s_ap_count], sizeof(rows[0]), "Other network...");
    items[s_ap_count] = rows[s_ap_count];
    deck_modal_list("JOIN NETWORK", items, s_ap_count + 1, network_picked, NULL);
}

static void join_clicked(lv_event_t *e)
{
    (void)e;
    if (s_ui.scan_poll) return;
    if (!net_scan_start()) {
        lv_label_set_text(lv_obj_get_child(s_ui.join_btn, 0), "NO RADIO");
        return;
    }
    lv_label_set_text(lv_obj_get_child(s_ui.join_btn, 0), "SCANNING");
    s_ui.scan_poll = lv_timer_create(scan_poll, 100, NULL);
}

static void forget_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (yes) net_forget();
}

static void forget_clicked(lv_event_t *e)
{
    (void)e;
    if (net_ssid()[0] == '\0') return;
    char msg[96];
    snprintf(msg, sizeof(msg), "Forget %s? The deck will stop joining it.", net_ssid());
    deck_modal_confirm("FORGET NETWORK", msg, "FORGET", forget_confirmed, NULL);
}

static void tz_picked(int index, void *ud)
{
    (void)ud;
    if (index >= 0) hal_tz_set(index);
}

static void tz_clicked(lv_event_t *e)
{
    (void)e;
    static const char *names[32];
    int n = hal_tz_count();
    for (int i = 0; i < n && i < 32; i++) names[i] = hal_tz_name(i);
    deck_modal_list("TIMEZONE", names, n, tz_picked, NULL);
}

/* "KEY ........ value" row; returns the value label. */
static lv_obj_t *kv_row(lv_obj_t *parent, const char *key)
{
    lv_obj_t *r = deck_box(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_t *k = deck_label(r, g_font.mono_m, g_pal.dim, key);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = deck_label(r, g_font.mono_m, g_pal.text, "");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    return v;
}

static void refresh_net(void)
{
    net_state_t st = net_state();
    lv_color_t c   = st == NET_CONNECTED ? g_pal.ok : st == NET_NO_RADIO ? g_pal.danger
                     : (st == NET_CONNECTING || st == NET_STARTING) ? g_pal.warn : g_pal.dim;
    lv_label_set_text(s_ui.net_state, net_state_name(st));
    lv_obj_set_style_text_color(s_ui.net_state, c, 0);
    lv_label_set_text(s_ui.net_ssid, net_ssid()[0] ? net_ssid() : "--");
    lv_label_set_text(s_ui.net_ip, net_ip()[0] ? net_ip() : "--");
    if (st == NET_CONNECTED) {
        lv_label_set_text_fmt(s_ui.net_signal, "%d dBm  %.*s", net_rssi(), net_bars(), "||||");
    } else {
        lv_label_set_text(s_ui.net_signal, "--");
    }
    lv_label_set_text(s_ui.tz_val, hal_tz_name(hal_tz_get()));
    lv_label_set_text(s_ui.ntp, net_time_synced() ? "NTP: SYNCED" : st == NET_CONNECTED ? "NTP: SYNCING"
                                                                                        : "NTP: WAITING FOR NETWORK");
    lv_obj_set_style_text_color(s_ui.ntp, net_time_synced() ? g_pal.ok : g_pal.dim, 0);
}

/* ---- Firmware updates ---------------------------------------------------- */

/* The install screen lives on the top layer so it survives leaving SYSTEM;
 * a timer of its own keeps it current until the deck reboots. */
static lv_obj_t *s_flash;
static lv_obj_t *s_flash_bar;
static lv_obj_t *s_flash_pct;
static lv_obj_t *s_flash_msg;

static void flash_tick(lv_timer_t *t)
{
    ota_state_t st = ota_state();
    lv_bar_set_value(s_flash_bar, ota_progress(), LV_ANIM_ON);
    lv_label_set_text_fmt(s_flash_pct, "%d%%", ota_progress());
    if (st == OTA_REBOOTING) {
        lv_label_set_text(s_flash_msg, "VERIFIED // REBOOTING INTO NEW BUILD");
        lv_obj_set_style_text_color(s_flash_msg, g_pal.ok, 0);
    } else if (st == OTA_ERROR) {
        /* Nothing was switched; the running build stays. */
        lv_timer_delete(t);
        lv_obj_delete(s_flash);
        s_flash = NULL;
        char msg[128];
        snprintf(msg, sizeof(msg), "Update failed: %s. Nothing was changed.", ota_error());
        deck_modal_confirm("UPDATE", msg, "OK", NULL, NULL);
    }
}

static void show_flash_screen(void)
{
    s_flash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_flash);
    lv_obj_set_size(s_flash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_flash, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(s_flash); /* under the scanlines */

    lv_obj_t *col = deck_box(s_flash);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 18, 0);
    lv_obj_center(col);
    lv_obj_t *t = deck_label(col, g_font.disp_l, g_pal.accent, "");
    lv_label_set_text_fmt(t, "FLASHING v%s", ota_latest());
    deck_label(col, g_font.mono_m, g_pal.dim, "WRITING TO THE IDLE SLOT // THE RUNNING BUILD STAYS UNTIL VERIFIED");
    s_flash_bar = lv_bar_create(col);
    lv_obj_set_size(s_flash_bar, 760, 22);
    lv_bar_set_range(s_flash_bar, 0, 100);
    lv_obj_set_style_radius(s_flash_bar, 0, 0);
    lv_obj_set_style_radius(s_flash_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_flash_bar, g_pal.panel_hi, 0);
    lv_obj_set_style_bg_color(s_flash_bar, g_pal.accent, LV_PART_INDICATOR);
    s_flash_pct = deck_label(col, g_font.disp_xl, g_pal.text, "0%");
    s_flash_msg = deck_label(col, g_font.mono_m, g_pal.warn, "DO NOT POWER OFF");
    lv_timer_create(flash_tick, 200, NULL);
}

static void install_confirmed(bool yes, void *ud)
{
    (void)ud;
    if (!yes || ota_state() != OTA_AVAILABLE) return;
    ota_install();
    show_flash_screen();
}

static void ota_clicked(lv_event_t *e)
{
    (void)e;
    ota_state_t st = ota_state();
    if (st == OTA_AVAILABLE) {
        char msg[400];
        const char *notes = ota_notes();
        snprintf(msg, sizeof(msg), "Install v%s? The deck reboots when done.\n\n%.300s%s", ota_latest(), notes,
                 strlen(notes) > 300 ? "..." : "");
        deck_modal_confirm("UPDATE", msg, "INSTALL", install_confirmed, NULL);
    } else if (st != OTA_CHECKING && st != OTA_DOWNLOADING && st != OTA_REBOOTING) {
        ota_check(hal_cfg_get_i32("ota_beta", 0) != 0);
    }
}

static void beta_changed(lv_event_t *e)
{
    bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    hal_cfg_set_i32("ota_beta", on);
}

static void refresh_ota(void)
{
    char buf[96];
    lv_color_t c = g_pal.dim;
    const char *btn = "CHECK";
    switch (ota_state()) {
        case OTA_IDLE:
            snprintf(buf, sizeof(buf), ota_pending_verify() ? "NEW BUILD: SELF-TEST RUNNING" : "RUNNING v%s",
                     ota_running());
            c = ota_pending_verify() ? g_pal.warn : g_pal.dim;
            break;
        case OTA_CHECKING:
            snprintf(buf, sizeof(buf), "CHECKING GITHUB...");
            c = g_pal.accent;
            break;
        case OTA_UP_TO_DATE:
            snprintf(buf, sizeof(buf), "UP TO DATE (v%s)", ota_running());
            c = g_pal.ok;
            break;
        case OTA_AVAILABLE:
            snprintf(buf, sizeof(buf), "v%s AVAILABLE", ota_latest());
            c   = g_pal.accent2;
            btn = "INSTALL";
            break;
        case OTA_DOWNLOADING:
            snprintf(buf, sizeof(buf), "DOWNLOADING %d%%", ota_progress());
            c = g_pal.accent;
            break;
        case OTA_REBOOTING: snprintf(buf, sizeof(buf), "REBOOTING..."); break;
        case OTA_ERROR:
            snprintf(buf, sizeof(buf), "%.80s", ota_error());
            c = g_pal.danger;
            break;
    }
    lv_label_set_text(s_ui.ota_status, buf);
    lv_obj_set_style_text_color(s_ui.ota_status, c, 0);
    lv_label_set_text(lv_obj_get_child(s_ui.ota_btn, 0), btn);
}

/* ---- Live info ----------------------------------------------------------- */

static void fmt_bytes(char *buf, size_t n, size_t used, size_t total)
{
    snprintf(buf, n, "%lu / %lu KB", (unsigned long)(used / 1024), (unsigned long)(total / 1024));
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    hal_sysinfo_t si;
    hal_sysinfo(&si);
    char buf[64];

    lv_label_set_text(s_ui.info[INFO_BOARD], si.board);
    snprintf(buf, sizeof(buf), "%s @ %luMHz", si.chip, (unsigned long)si.cpu_mhz);
    lv_label_set_text(s_ui.info[INFO_CHIP], buf);
    fmt_bytes(buf, sizeof(buf), si.psram_total - si.psram_free, si.psram_total);
    lv_label_set_text(s_ui.info[INFO_PSRAM], buf);
    fmt_bytes(buf, sizeof(buf), si.heap_int_total - si.heap_int_free, si.heap_int_total);
    lv_label_set_text(s_ui.info[INFO_SRAM], buf);
    lv_label_set_text_fmt(s_ui.info[INFO_UPTIME], "%luh %02lum %02lus", (unsigned long)(si.uptime_s / 3600),
                          (unsigned long)(si.uptime_s / 60 % 60), (unsigned long)(si.uptime_s % 60));

    if (hal_kbd_present()) {
        lv_label_set_text_fmt(s_ui.info[INFO_KBD], "LINKED  FW 0x%02X", hal_kbd_fw_version());
    } else {
        lv_label_set_text(s_ui.info[INFO_KBD], "NOT DETECTED");
    }

    if (hal_sd_mounted()) {
        snprintf(buf, sizeof(buf), "SD %.1f / %.1f GB FREE", (double)hal_sd_free_bytes() / 1e9,
                 (double)hal_sd_total_bytes() / 1e9);
        lv_label_set_text(s_ui.info[INFO_SD], buf);
    } else {
        lv_label_set_text(s_ui.info[INFO_SD], "NO MEDIA");
    }

    hal_power_t p;
    hal_power_read(&p);
    if (!p.valid) {
        lv_label_set_text(s_ui.info[INFO_PWR], "NO SENSOR");
    } else if (p.percent < 0) {
        snprintf(buf, sizeof(buf), "USB  %.2fV", (double)p.volts);
        lv_label_set_text(s_ui.info[INFO_PWR], buf);
    } else {
        snprintf(buf, sizeof(buf), "%.2fV %+.0fmA %d%%%s", (double)p.volts, (double)(p.amps * 1000.0f), p.percent,
                 p.charging ? " CHG" : "");
        lv_label_set_text(s_ui.info[INFO_PWR], buf);
    }
    lv_label_set_text(s_ui.info[INFO_FW], "DECK//OS v" DECK_VERSION);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_ui.clock_val, "%04d-%02d-%02d  %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                          tm.tm_mday, tm.tm_hour, tm.tm_min);
    refresh_net();
    refresh_ota();
}

/* ---- Module -------------------------------------------------------------- */

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = deck_button(parent, text, NULL);
    lv_obj_set_style_min_height(b, 46, 0);
    lv_obj_set_style_pad_ver(b, 8, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    s_ui = (sys_ui_t){0};

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_column(root, 20, 0);

    /* ---- DISPLAY ---- */
    lv_obj_t *left = column_panel(root, 372);
    deck_section(left, "DISPLAY");

    lv_obj_t *row = setting_row(left, ICON_BRIGHTNESS, "BRIGHTNESS");
    s_ui.bright_val = deck_label(row, g_font.mono_m, g_pal.accent, "");
    lv_obj_align(s_ui.bright_val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *slider = lv_slider_create(left);
    lv_obj_set_width(slider, LV_PCT(92));
    lv_obj_set_style_margin_left(slider, 10, 0);
    lv_obj_set_style_margin_bottom(slider, 8, 0);
    lv_slider_set_range(slider, 10, 100);
    int32_t bright = hal_cfg_get_i32("bright", 80);
    lv_slider_set_value(slider, bright, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ui.bright_val, "%ld%%", (long)bright);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_DEFOCUSED, NULL);

    row = setting_row(left, ICON_MONITOR, "SCANLINES");
    add_switch(row, hal_cfg_get_i32("scan", 1) != 0, scan_changed);
    row = setting_row(left, ICON_CHIP, "BOOT POST");
    add_switch(row, hal_cfg_get_i32("boot", 1) != 0, boot_changed);

    deck_section(left, "ACCENT");
    lv_obj_t *sw = deck_box(left);
    lv_obj_set_width(sw, LV_PCT(100));
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sw, 12, 0);
    for (int i = 0; i < DECK_ACCENT_COUNT; i++) {
        lv_obj_t *b = deck_panel(sw, DECK_CUT_BR, 12);
        lv_obj_set_size(b, 150, 58);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        deck_panel_set_outline(b, deck_theme_accent_color(i));
        if (i == deck_theme_accent()) {
            lv_obj_add_state(b, LV_STATE_CHECKED);
            deck_panel_set_fill(b, g_pal.panel_hi);
        }
        lv_obj_t *bar = lv_obj_create(b);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 110, 5);
        lv_obj_set_style_bg_color(bar, deck_theme_accent_color(i), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_t *l = deck_label(b, g_font.mono_s, deck_theme_accent_color(i), deck_theme_accent_name(i));
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_add_event_cb(b, accent_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(deck_input_group(), b);
    }

    /* ---- NETWORK + TIME ---- */
    lv_obj_t *mid = column_panel(root, 392);
    lv_obj_set_style_pad_row(mid, 10, 0);
    deck_section(mid, "NETWORK");
    s_ui.net_state  = deck_label(mid, g_font.disp_m, g_pal.dim, "");
    s_ui.net_ssid   = kv_row(mid, "SSID");
    s_ui.net_ip     = kv_row(mid, "IP");
    s_ui.net_signal = kv_row(mid, "SIGNAL");
    lv_obj_t *nb = deck_box(mid);
    lv_obj_set_width(nb, LV_PCT(100));
    lv_obj_set_flex_flow(nb, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nb, 12, 0);
    lv_obj_set_style_margin_top(nb, 4, 0);
    s_ui.join_btn = small_button(nb, "JOIN", join_clicked, NULL);
    small_button(nb, "FORGET", forget_clicked, NULL);

    lv_obj_t *ts = deck_section(mid, "TIME");
    lv_obj_set_style_margin_top(ts, 8, 0);
    lv_obj_t *tzr = deck_box(mid);
    lv_obj_set_width(tzr, LV_PCT(100));
    s_ui.tz_val = deck_label(tzr, g_font.mono_m, g_pal.text, "");
    lv_obj_align(s_ui.tz_val, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *zb = small_button(tzr, "ZONE", tz_clicked, NULL);
    lv_obj_align(zb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *clk = deck_box(mid);
    lv_obj_set_width(clk, LV_PCT(100));
    s_ui.clock_val = deck_label(clk, g_font.mono_m, g_pal.text, "");
    lv_obj_align(s_ui.clock_val, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *set = small_button(clk, "SET", open_clock_modal, parent);
    lv_obj_align(set, LV_ALIGN_RIGHT_MID, 0, 0);
    s_ui.ntp = deck_label(mid, g_font.mono_s, g_pal.dim, "");

    /* ---- SYSTEM ---- */
    lv_obj_t *right = column_panel(root, 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_row(right, 8, 0);
    deck_section(right, "SYSTEM");
    for (int i = 0; i < INFO_COUNT; i++) s_ui.info[i] = kv_row(right, s_info_keys[i]);

    lv_obj_t *us = deck_section(right, "UPDATE");
    lv_obj_set_style_margin_top(us, 10, 0);
    s_ui.ota_status = deck_label(right, g_font.mono_m, g_pal.dim, "");
    lv_obj_t *urow  = deck_box(right);
    lv_obj_set_width(urow, LV_PCT(100));
    s_ui.ota_btn = small_button(urow, "CHECK", ota_clicked, NULL);
    lv_obj_align(s_ui.ota_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *bl = deck_label(urow, g_font.mono_s, g_pal.dim, "BETA");
    lv_obj_align(bl, LV_ALIGN_RIGHT_MID, -90, 0);
    add_switch(urow, hal_cfg_get_i32("ota_beta", 0) != 0, beta_changed);

    refresh(NULL);
    s_ui.timer = lv_timer_create(refresh, 1000, NULL);
    lv_group_focus_obj(slider);
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
    if (s_ui.scan_poll) lv_timer_delete(s_ui.scan_poll);
    s_ui = (sys_ui_t){0};
}

static bool on_key(deck_app_t *self, const deck_key_t *k)
{
    (void)self;
    if (s_ui.modal && k->code == HID_ESC) {
        modal_close();
        return true;
    }
    return false;
}

static deck_app_t s_app = {
    .name     = "SYSTEM",
    .tagline  = "DECK CONFIG",
    .icon     = ICON_COG,
    .eta      = NULL,
    .on_start = start,
    .on_stop  = stop,
    .on_key   = on_key,
};

deck_app_t *app_sys(void) { return &s_app; }
