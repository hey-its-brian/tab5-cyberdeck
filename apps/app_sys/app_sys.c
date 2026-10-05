/*
 * SYSTEM module: display and style settings, clock, live hardware readout.
 */
#include "app_sys.h"

#include <stdio.h>
#include <time.h>

#include "deck_fx.h"
#include "deck_hal.h"
#include "deck_icons.h"
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
        snprintf(buf, sizeof(buf), "SD %.2f / %.2f GB FREE", (double)hal_sd_free_bytes() / 1e9,
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
        snprintf(buf, sizeof(buf), "%.2fV  %+.0fmA  %d%%%s", (double)p.volts, (double)(p.amps * 1000.0f), p.percent,
                 p.charging ? "  CHG" : "");
        lv_label_set_text(s_ui.info[INFO_PWR], buf);
    }
    lv_label_set_text(s_ui.info[INFO_FW], "DECK//OS v" DECK_VERSION);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    lv_label_set_text_fmt(s_ui.clock_val, "%04d-%02d-%02d  %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                          tm.tm_mday, tm.tm_hour, tm.tm_min);
}

/* ---- Module -------------------------------------------------------------- */

static bool start(deck_app_t *self, lv_obj_t *parent)
{
    (void)self;
    s_ui = (sys_ui_t){0};

    lv_obj_t *root = deck_box(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root, 24, 0);
    lv_obj_set_style_pad_column(root, 24, 0);

    /* Left: settings */
    lv_obj_t *left = column_panel(root, 620);
    deck_section(left, "DISPLAY");

    lv_obj_t *row = setting_row(left, ICON_BRIGHTNESS, "BRIGHTNESS");
    s_ui.bright_val = deck_label(row, g_font.mono_m, g_pal.accent, "");
    lv_obj_align(s_ui.bright_val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *slider = lv_slider_create(left);
    lv_obj_set_width(slider, LV_PCT(96));
    lv_obj_set_style_margin_left(slider, 10, 0);
    lv_slider_set_range(slider, 10, 100);
    int32_t bright = hal_cfg_get_i32("bright", 80);
    lv_slider_set_value(slider, bright, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_ui.bright_val, "%ld%%", (long)bright);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(slider, bright_changed, LV_EVENT_DEFOCUSED, NULL);

    row = setting_row(left, ICON_MONITOR, "SCANLINES");
    add_switch(row, hal_cfg_get_i32("scan", 1) != 0, scan_changed);
    row = setting_row(left, ICON_CHIP, "BOOT POST SEQUENCE");
    add_switch(row, hal_cfg_get_i32("boot", 1) != 0, boot_changed);

    deck_section(left, "ACCENT");
    lv_obj_t *sw = deck_box(left);
    lv_obj_set_width(sw, LV_PCT(100));
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sw, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < DECK_ACCENT_COUNT; i++) {
        lv_obj_t *b = deck_panel(sw, DECK_CUT_BR, 12);
        lv_obj_set_size(b, 128, 64);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        deck_panel_set_outline(b, deck_theme_accent_color(i));
        if (i == deck_theme_accent()) {
            lv_obj_add_state(b, LV_STATE_CHECKED);
            deck_panel_set_fill(b, g_pal.panel_hi);
        }
        lv_obj_t *bar = lv_obj_create(b);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 96, 6);
        lv_obj_set_style_bg_color(bar, deck_theme_accent_color(i), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_t *l = deck_label(b, g_font.mono_s, deck_theme_accent_color(i), deck_theme_accent_name(i));
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_obj_add_event_cb(b, accent_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(deck_input_group(), b);
    }

    deck_section(left, "CLOCK");
    row = setting_row(left, ICON_CLOCK, "");
    s_ui.clock_val = lv_obj_get_child(lv_obj_get_child(row, 0), 1);
    lv_obj_t *set = deck_button(row, "SET", NULL);
    lv_obj_align(set, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(set, open_clock_modal, LV_EVENT_CLICKED, parent);

    /* Right: live system info */
    lv_obj_t *right = column_panel(root, 0);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_style_pad_row(right, 8, 0);
    deck_section(right, "SYSTEM");
    for (int i = 0; i < INFO_COUNT; i++) {
        lv_obj_t *r = deck_box(right);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_t *k = deck_label(r, g_font.mono_m, g_pal.dim, s_info_keys[i]);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
        s_ui.info[i] = deck_label(r, g_font.mono_m, g_pal.text, "");
        lv_obj_align(s_ui.info[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    static const char *hotkeys[][2] = {
        {"ALT+1..5", "LAUNCH MODULE"},
        {"ALT+ESC", "RETURN TO DECK, ALWAYS"},
        {"ESC", "BACK / CLOSE"},
        {"TAB  ARROWS", "MOVE FOCUS"},
        {"ENTER", "ACTIVATE"},
    };
    lv_obj_t *hk = deck_section(right, "HOTKEYS");
    lv_obj_set_style_margin_top(hk, 10, 0);
    for (size_t i = 0; i < sizeof(hotkeys) / sizeof(hotkeys[0]); i++) {
        lv_obj_t *r = deck_box(right);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_t *k = deck_label(r, g_font.mono_m, g_pal.accent, hotkeys[i][0]);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *v = deck_label(r, g_font.mono_m, g_pal.dim, hotkeys[i][1]);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    refresh(NULL);
    s_ui.timer = lv_timer_create(refresh, 1000, NULL);
    lv_group_focus_obj(slider);
    return true;
}

static void stop(deck_app_t *self)
{
    (void)self;
    if (s_ui.timer) lv_timer_delete(s_ui.timer);
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
