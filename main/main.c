/*
 * DECK//OS entry point: bring up the hardware, then hand the screen to the
 * shell. Everything after that runs on the LVGL task.
 */
#include "deck_hal.h"
#include "deck_net.h"
#include "deck_ota.h"
#include "deck_input.h"
#include "deck_shell.h"
#include "deck_theme.h"

#include "app_calc.h"
#include "app_notes.h"
#include "app_player.h"
#include "deck_audio.h"
#include "app_files.h"
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

    /* Launcher order: tile 01..07, Alt+1..7. SYSTEM stays last. */
    deck_app_register(app_term());
    deck_app_register(app_notes());
    deck_app_register(app_calc());
    deck_app_register(app_weather());
    deck_app_register(app_player());
    deck_app_register(app_files());
    deck_app_register(app_sys());

    deck_shell_start(hal_cfg_get_i32("boot", 1) != 0);
    hal_lvgl_unlock();
}
