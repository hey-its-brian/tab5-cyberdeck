/*
 * HUD status bar: brand or back button on the left, clock in the middle,
 * hardware indicators on the right. Refreshes once a second.
 */
#include "deck_statusbar.h"

#include <stdio.h>
#include <time.h>

#include "deck_hal.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_portal.h"
#include "deck_icons.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

static lv_obj_t *s_bar;
static lv_obj_t *s_left;
static lv_obj_t *s_clock;
static lv_obj_t *s_date;
static lv_obj_t *s_kbd;
static lv_obj_t *s_sd;
static lv_obj_t *s_net;
static lv_obj_t *s_pwr;
static lv_obj_t *s_upd;
static bool s_auto_checked; /* one update check per boot, once online */
static lv_obj_t *s_link;
static lv_timer_t *s_timer;

static void back_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_home();
}

static void clock_clicked(lv_event_t *e)
{
    (void)e;
    deck_shell_sleep();
}

static void build_left(deck_app_t *app)
{
    lv_obj_clean(s_left);
    if (app == NULL) {
        lv_obj_t *brand = deck_box(s_left);
        lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
        deck_label(brand, g_font.disp_m, g_pal.text, "DECK");
        deck_label(brand, g_font.disp_m, g_pal.accent, "//OS");
        lv_obj_t *ver = deck_label(s_left, g_font.mono_s, g_pal.dim, "v" DECK_VERSION);
        lv_obj_set_style_pad_left(ver, 10, 0);
        return;
    }
    lv_obj_t *back = deck_button(s_left, "DECK", g_font.disp_s);
    /* Prepend a chevron icon to the button's label. */
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(back, 4, 0);
    lv_obj_t *chev = lv_label_create(back);
    lv_obj_set_style_text_font(chev, g_font.icon_s, 0);
    lv_label_set_text(chev, ICON_CHEVRON_LEFT);
    lv_obj_move_to_index(chev, 0);
    lv_obj_set_style_min_height(back, 44, 0);
    lv_obj_set_style_pad_ver(back, 6, 0);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);
    /* Keyboard users have Esc; keep the back button out of Tab order. */
    lv_group_remove_obj(back);

    lv_obj_t *title = deck_label(s_left, g_font.disp_m, g_pal.text, "");
    lv_label_set_text_fmt(title, "%02d", deck_app_index(app) + 1);
    lv_obj_set_style_text_color(title, g_pal.accent, 0);
    lv_obj_set_style_pad_left(title, 14, 0);
    deck_label(s_left, g_font.disp_m, g_pal.dim, "//");
    deck_label(s_left, g_font.disp_m, g_pal.text, app->name);
}

static void set_indicator(lv_obj_t *row, const char *icon, const char *text, lv_color_t color)
{
    deck_icon_text_set(row, icon, text, color);
}

static void refresh(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    static const char *days[]   = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    lv_label_set_text_fmt(s_clock, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    lv_label_set_text_fmt(s_date, "%s %02d %s %04d", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
                          tm.tm_year + 1900);

    bool kbd = hal_kbd_present();
    set_indicator(s_kbd, kbd ? ICON_KEYBOARD : ICON_KEYBOARD_OFF, "KBD", kbd ? g_pal.accent : g_pal.dim);
    set_indicator(s_sd, ICON_SD, "SD", hal_sd_mounted() ? g_pal.accent : g_pal.dim);
    if (!s_auto_checked && net_state() == NET_CONNECTED && net_time_synced()) {
        s_auto_checked = true;
        ota_check(hal_cfg_get_i32("ota_beta", 0) != 0);
    }
    if (ota_state() == OTA_AVAILABLE) {
        lv_obj_remove_flag(s_upd, LV_OBJ_FLAG_HIDDEN);
        set_indicator(s_upd, ICON_BOLT, "UPD", (tm.tm_sec & 1) ? g_pal.accent2 : g_pal.dim);
    } else {
        lv_obj_add_flag(s_upd, LV_OBJ_FLAG_HIDDEN);
    }

    switch (net_state()) {
        case NET_CONNECTED: set_indicator(s_net, ICON_WIFI, "NET", g_pal.accent); break;
        case NET_CONNECTING:
        case NET_STARTING:
            /* Blink while joining. */
            set_indicator(s_net, ICON_WIFI, "NET", (tm.tm_sec & 1) ? g_pal.warn : g_pal.dim);
            break;
        default: set_indicator(s_net, ICON_WIFI_OFF, "NET", g_pal.dim); break;
    }

    /* The portal outlives its module, so its idle shutdown is driven here. */
    portal_tick();
    if (portal_running()) {
        lv_obj_remove_flag(s_link, LV_OBJ_FLAG_HIDDEN);
        set_indicator(s_link, ICON_SERVER, "LINK", portal_busy() && (tm.tm_sec & 1) ? g_pal.text : g_pal.accent2);
    } else {
        lv_obj_add_flag(s_link, LV_OBJ_FLAG_HIDDEN);
    }

    hal_power_t p;
    hal_power_read(&p);
    char buf[32];
    if (!p.valid) {
        set_indicator(s_pwr, ICON_BATTERY_EMPTY, "--", g_pal.dim);
    } else if (p.percent < 0) {
        set_indicator(s_pwr, ICON_PLUG, "USB", g_pal.accent);
    } else {
        snprintf(buf, sizeof(buf), "%d%%", p.percent);
        set_indicator(s_pwr, p.charging ? ICON_BATTERY_CHG : ICON_BATTERY, buf,
                      p.percent <= 15 && !p.charging ? g_pal.danger : g_pal.accent);
    }
}

static lv_obj_t *indicator(lv_obj_t *parent)
{
    return deck_icon_text(parent, "", "", g_font.mono_m, g_pal.dim);
}

void deck_statusbar_create(lv_obj_t *parent)
{
    s_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_bar);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bar, LV_PCT(100), DECK_STATUSBAR_H);
    lv_obj_set_style_bg_color(s_bar, g_pal.panel, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);

    /* Accent rule along the bottom with a brighter segment on the left. */
    lv_obj_t *rule = lv_obj_create(s_bar);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, LV_PCT(100), 2);
    lv_obj_set_style_bg_color(rule, g_pal.accent, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_40, 0);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *seg = lv_obj_create(s_bar);
    lv_obj_remove_style_all(seg);
    lv_obj_set_size(seg, 220, 3);
    lv_obj_set_style_bg_color(seg, g_pal.accent, 0);
    lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
    lv_obj_add_flag(seg, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(seg, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *seg2 = lv_obj_create(s_bar);
    lv_obj_remove_style_all(seg2);
    lv_obj_set_size(seg2, 60, 3);
    lv_obj_set_style_bg_color(seg2, g_pal.accent2, 0);
    lv_obj_set_style_bg_opa(seg2, LV_OPA_COVER, 0);
    lv_obj_add_flag(seg2, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(seg2, LV_ALIGN_BOTTOM_LEFT, 230, 0);

    s_left = deck_box(s_bar);
    lv_obj_set_height(s_left, LV_PCT(100));
    lv_obj_set_flex_flow(s_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_left, 8, 0);
    lv_obj_align(s_left, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *mid = deck_box(s_bar);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(mid, 14, 0);
    lv_obj_align(mid, LV_ALIGN_CENTER, 0, 0);
    s_clock = deck_label(mid, g_font.disp_m, g_pal.text, "--:--:--");
    /* Tap the clock to sleep the screen. */
    lv_obj_add_flag(mid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(mid, 12);
    lv_obj_add_event_cb(mid, clock_clicked, LV_EVENT_CLICKED, NULL);
    s_date  = deck_label(mid, g_font.mono_s, g_pal.dim, "");

    lv_obj_t *right = deck_box(s_bar);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 22, 0);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -16, 0);
    s_upd = indicator(right); /* shown only while an update is waiting */
    lv_obj_add_flag(s_upd, LV_OBJ_FLAG_HIDDEN);
    s_link = indicator(right);
    lv_obj_add_flag(s_link, LV_OBJ_FLAG_HIDDEN);
    s_kbd = indicator(right);
    s_sd  = indicator(right);
    s_net = indicator(right);
    s_pwr = indicator(right);

    build_left(NULL);
    refresh(NULL);
    s_timer = lv_timer_create(refresh, 1000, NULL);
}

void deck_statusbar_set_app(deck_app_t *app)
{
    if (s_left) build_left(app);
}

void deck_statusbar_destroy(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_bar) {
        lv_obj_delete(s_bar);
        s_bar = s_left = NULL;
    }
}
