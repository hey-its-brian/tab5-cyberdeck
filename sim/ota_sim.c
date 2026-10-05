/* deck_ota for the simulator: walks through check / download / reboot so
 * the update UI can be exercised. Set SIM_OTA_NEWER=0 for "up to date". */
#include "deck_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"

static ota_state_t s_state = OTA_IDLE;
static uint32_t s_since;
static char s_running[24];

void ota_init(const char *v) { snprintf(s_running, sizeof(s_running), "%s", v); }

static void advance(void)
{
    uint32_t t = lv_tick_elaps(s_since);
    if (s_state == OTA_CHECKING && t > 700) {
        const char *e = getenv("SIM_OTA_NEWER");
        s_state       = (e && e[0] == '0') ? OTA_UP_TO_DATE : OTA_AVAILABLE;
    } else if (s_state == OTA_DOWNLOADING && t > 3000) {
        s_state = OTA_REBOOTING;
    }
}

void ota_check(bool pre)
{
    (void)pre;
    s_state = OTA_CHECKING;
    s_since = lv_tick_get();
}

void ota_install(void)
{
    if (s_state != OTA_AVAILABLE) return;
    s_state = OTA_DOWNLOADING;
    s_since = lv_tick_get();
}

ota_state_t ota_state(void)
{
    advance();
    return s_state;
}

const char *ota_running(void) { return s_running; }
const char *ota_latest(void) { return "0.5.1"; }
const char *ota_notes(void) { return "Bug fixes and a faster boot."; }
const char *ota_error(void) { return ""; }
int ota_progress(void)
{
    int p = (int)(lv_tick_elaps(s_since) / 30);
    return s_state == OTA_DOWNLOADING ? (p > 100 ? 100 : p) : (s_state == OTA_REBOOTING ? 100 : 0);
}
bool ota_just_updated(void) { return false; }
bool ota_pending_verify(void) { return false; }
