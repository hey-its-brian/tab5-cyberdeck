/*
 * DECK//OS entry point: bring up the hardware, then hand the screen to the
 * shell. Everything after that runs on the LVGL task.
 */
#include "deck_hal.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_input.h"
#include "deck_home.h"
#include "deck_icons.h"
#include "deck_shell.h"
#include "deck_sync.h"
#include "deck_theme.h"

#include "app_calc.h"
#include "app_notes.h"
#include "app_player.h"
#include "deck_audio.h"
#include "app_files.h"
#include "app_net.h"
#include "app_sys.h"
#include "app_term.h"
#include "app_weather.h"

#include "esp_log.h"

void app_main(void)
{
    if (!hal_init()) {
        ESP_LOGE("main", "display bring-up failed, halting");
        return;
    }

    net_init(); /* brings up the C6 link in the background */
    ota_init(DECK_VERSION); /* arms the rollback countdown on a fresh OTA build */

    player_init(); /* music task; idle until something plays */

    hal_lvgl_lock(0);
    deck_theme_init();
    deck_input_init();

    /* Every module, flat: Alt+letter shortcuts and lookups by name. */
    deck_app_register(app_term());
    deck_app_register(app_notes());
    deck_app_register(app_calc());
    deck_app_register(app_weather());
    deck_app_register(app_player());
    deck_app_register(app_files());
    deck_app_register(app_net());
    deck_app_register(app_sys());
    deck_sync_set_backend(&g_sync_livesync); /* vault sync for NOTES */

    /* Home tiles 01..05 (Alt+1..5). Groups open a screen of their own. */
    deck_group_t *tools = deck_home_add_group("TOOLS", "UTILITY STACK", ICON_TOOLS);
    deck_group_add(tools, app_term());
    deck_group_add(tools, app_calc());
    deck_group_add(tools, app_sys());
    deck_home_add_app(app_net());
    deck_group_t *media = deck_home_add_group("FILES & MEDIA", "STORAGE + AUDIO", ICON_MEDIA);
    deck_group_add(media, app_files());
    deck_group_add(media, app_player());
    deck_home_add_app(app_notes());
    deck_home_add_app(app_weather());

    deck_shell_start(hal_cfg_get_i32("boot", 1) != 0);
    hal_lvgl_unlock();
}
