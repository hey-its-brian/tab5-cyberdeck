/*
 * Update notifications: check GitHub at startup (once online and the clock
 * is set) and then hourly, and offer a newer build in a popup:
 *   LATER   (or Esc) ask again after the next check
 *   IGNORE  never pop up for this version (SYSTEM > UPDATE can still install)
 *   INSTALL flash it, with the progress screen
 * The popup waits for a good moment: not while the screen sleeps, a dialog
 * is open, TERMINAL is in front, or the portal is moving a file.
 */
#include "deck_update.h"

#include <stdio.h>
#include <string.h>

#include "deck_hal.h"
#include "deck_modal.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_portal.h"
#include "deck_saver.h"
#include "deck_shell.h"
#include "deck_theme.h"
#include "deck_widgets.h"

#define CHECK_EVERY_S (60 * 60)

static uint32_t s_last_check_s; /* uptime of the last automatic check */
static bool s_checked_once;
static bool s_auto_in_flight;   /* our check is running */
static bool s_offer_pending;    /* found something, waiting to show */

/* ---- Install screen ------------------------------------------------------ */

/* Lives on the top layer so it survives leaving the current module; its own
 * timer keeps it current until the deck reboots. */
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

void deck_update_install(void)
{
    if (ota_state() != OTA_AVAILABLE || s_flash) return;
    ota_install();

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

/* ---- Popup --------------------------------------------------------------- */

static void choice_made(int choice, void *ud)
{
    (void)ud;
    if (choice == 1) {
        deck_update_install();
    } else if (choice == 0) {
        hal_cfg_set_str("ota_skip", ota_latest()); /* until a newer one comes out */
    }
}

static bool good_moment(void)
{
    if (deck_shell_sleeping() || deck_saver_active() || deck_modal_active() || portal_busy() || s_flash) return false;
    deck_app_t *app = deck_shell_current();
    return app == NULL || strcmp(app->name, "TERMINAL") != 0;
}

static void offer(void)
{
    char msg[420];
    const char *notes = ota_notes();
    snprintf(msg, sizeof(msg), "v%s is available (running v%s).\n\n%.300s%s", ota_latest(), ota_running(), notes,
             strlen(notes) > 300 ? "..." : "");
    deck_modal_choice("UPDATE AVAILABLE", msg, "LATER", "IGNORE", "INSTALL", choice_made, NULL);
}

void deck_update_tick(void)
{
    uint32_t now = lv_tick_get() / 1000;
    bool online  = net_state() == NET_CONNECTED && net_time_synced();
    ota_state_t st = ota_state();

    /* Start a check: first one as soon as we're online, then hourly. */
    if (online && !s_auto_in_flight && st != OTA_CHECKING && st != OTA_DOWNLOADING && st != OTA_REBOOTING &&
        (!s_checked_once || now - s_last_check_s >= CHECK_EVERY_S)) {
        s_checked_once   = true;
        s_last_check_s   = now;
        s_auto_in_flight = true;
        s_offer_pending  = false;
        ota_check(hal_cfg_get_i32("ota_beta", 0) != 0);
        return;
    }

    /* Our check finished: anything to offer? */
    if (s_auto_in_flight && st != OTA_CHECKING) {
        s_auto_in_flight = false;
        char skip[24];
        hal_cfg_get_str("ota_skip", skip, sizeof(skip));
        s_offer_pending = st == OTA_AVAILABLE && strcmp(skip, ota_latest()) != 0;
    }

    if (s_offer_pending) {
        if (ota_state() != OTA_AVAILABLE) {
            s_offer_pending = false; /* installed from SYSTEM, or a newer check changed it */
        } else if (good_moment()) {
            s_offer_pending = false;
            offer();
        }
    }
}
