/*
 * Screensaver. Idle time comes from LVGL (touch and the keys it sees); the
 * shell also pokes it for every key, including the ones it consumes itself.
 *
 * The clock moves to a new random spot every 30 s so no pixel stays lit in
 * one place, and the backlight drops to a quarter of the user's brightness.
 */
#include "deck_saver.h"

#include <stdio.h>
#include <time.h>

#include "deck_audio.h"
#include "deck_boot.h"
#include "deck_hal.h"
#include "deck_icons.h"
#include "deck_ota.h"
#include "deck_portal.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define SLEEP_AFTER_MS (10u * 60u * 1000u) /* saver, then full screen sleep */
#define DRIFT_EVERY_MS (30u * 1000u)

static lv_obj_t *s_root;  /* overlay on the top layer while the saver runs */
static lv_obj_t *s_box, *s_time, *s_date, *s_track;
static lv_timer_t *s_timer;
static uint32_t s_started, s_moved;

void deck_saver_poke(void) { lv_display_trigger_activity(NULL); }
bool deck_saver_active(void) { return s_root != NULL; }

int deck_saver_timeout(void)
{
    int m = (int)hal_cfg_get_i32("saver", 5);
    return m < 0 ? 0 : m;
}

void deck_saver_set_timeout(int minutes)
{
    hal_cfg_set_i32("saver", minutes);
    deck_saver_poke(); /* count from now, not from the last touch */
}

static void drift(void)
{
    lv_obj_update_layout(s_box);
    int32_t w = lv_obj_get_width(s_box), h = lv_obj_get_height(s_box);
    int32_t maxx = 1280 - 40 - w, maxy = 720 - 40 - h;
    lv_obj_set_pos(s_box, (int32_t)lv_rand(40, (uint32_t)(maxx > 40 ? maxx : 40)),
                   (int32_t)lv_rand(40, (uint32_t)(maxy > 40 ? maxy : 40)));
    s_moved = lv_tick_get();
}

static void update(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[32];
    strftime(b, sizeof(b), "%H:%M", &tm);
    lv_label_set_text(s_time, b);
    static const char *days[]   = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                   "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    lv_label_set_text_fmt(s_date, "%s %02d %s %04d", days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
                          tm.tm_year + 1900);
    player_state_t ps = player_state();
    if (ps == PLAYER_STOPPED) {
        lv_obj_add_flag(s_track, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_track, LV_OBJ_FLAG_HIDDEN);
        deck_icon_text_set(s_track, ps == PLAYER_PLAYING ? ICON_MUSIC : ICON_PAUSE, player_title(), g_pal.accent);
    }
    if (lv_tick_elaps(s_moved) >= DRIFT_EVERY_MS) drift();
}

static void close_saver(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_root) lv_obj_delete(s_root);
    s_root = NULL;
    hal_backlight_set((uint8_t)hal_cfg_get_i32("bright", 80));
}

static void wake_async(void *u)
{
    (void)u;
    close_saver();
    deck_saver_poke();
}

void deck_saver_wake(void)
{
    if (s_root) wake_async(NULL);
}

static void pressed(lv_event_t *e)
{
    (void)e;
    lv_async_call(wake_async, NULL); /* not from inside the overlay's own event */
}

static void open_saver(void)
{
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_root, pressed, LV_EVENT_PRESSED, NULL);

    s_box = deck_box(s_root);
    lv_obj_set_size(s_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_box, 10, 0);
    s_time  = deck_label(s_box, g_font.disp_hero, g_pal.accent, "");
    s_date  = deck_label(s_box, g_font.mono_l, g_pal.dim, "");
    s_track = deck_icon_text(s_box, ICON_MUSIC, "", g_font.mono_m, g_pal.accent);

    uint8_t bright = (uint8_t)hal_cfg_get_i32("bright", 80);
    hal_backlight_set((uint8_t)(bright / 4 < 10 ? 10 : bright / 4));
    s_started = lv_tick_get();
    update(NULL);
    drift();
    s_timer = lv_timer_create(update, 1000, NULL);
}

void deck_saver_tick(void)
{
    if (s_root) {
        if (lv_tick_elaps(s_started) >= SLEEP_AFTER_MS) {
            close_saver();
            deck_shell_sleep(); /* its own overlay; any key or touch wakes */
        }
        return;
    }
    int minutes = deck_saver_timeout();
    if (minutes == 0) return;
    if (portal_busy()) {
        deck_saver_poke(); /* a transfer counts as activity */
        return;
    }
    ota_state_t ota = ota_state();
    if (deck_shell_sleeping() || deck_boot_active() || ota == OTA_DOWNLOADING || ota == OTA_REBOOTING) return;
    if (lv_display_get_inactive_time(NULL) >= (uint32_t)minutes * 60u * 1000u) open_saver();
}
