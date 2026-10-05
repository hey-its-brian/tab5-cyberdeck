/*
 * deck_hal for the desktop simulator: plausible fake hardware so every
 * screen renders as it would on the Tab5.
 */
#include "deck_hal.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "sim.h"

#define KEYQ 64
static hal_key_t s_keys[KEYQ];
static int s_khead, s_ktail;

static struct {
    char key[16];
    int32_t val;
} s_cfg[32];
static int s_cfg_n;

static uint8_t s_backlight = 80;

bool hal_init(void) { return true; }
bool hal_lvgl_lock(uint32_t timeout_ms) { return true; }
void hal_lvgl_unlock(void) {}

void hal_backlight_set(uint8_t percent) { s_backlight = percent; }

void sim_push_key(uint8_t code, uint8_t mods, bool pressed)
{
    int next = (s_ktail + 1) % KEYQ;
    if (next == s_khead) return;
    s_keys[s_ktail] = (hal_key_t){pressed ? code : 0, mods, pressed};
    s_ktail         = next;
}

bool hal_key_poll(hal_key_t *out)
{
    if (s_khead == s_ktail) return false;
    *out    = s_keys[s_khead];
    s_khead = (s_khead + 1) % KEYQ;
    return true;
}

bool hal_kbd_present(void) { return true; }
uint8_t hal_kbd_fw_version(void) { return 0x12; }

void hal_power_read(hal_power_t *out)
{
    out->valid    = true;
    out->volts    = 7.86f;
    out->amps     = -0.412f;
    out->percent  = 78;
    out->charging = false;
}

bool hal_rtc_set(const struct tm *t)
{
    printf("[sim] clock set to %04d-%02d-%02d %02d:%02d (not applied to host)\n", t->tm_year + 1900,
           t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min);
    return true;
}

bool hal_rtc_present(void) { return true; }

/* Notes live in sim/sdcard (created on demand). */
const char *hal_storage_root(void) { return DECK_SIM_SDCARD; }
bool hal_storage_is_sd(void) { return true; }

bool hal_sd_mounted(void) { return true; }
uint64_t hal_sd_total_bytes(void) { return 31914983424ull; }
uint64_t hal_sd_free_bytes(void) { return 29716070400ull; }

int32_t hal_cfg_get_i32(const char *key, int32_t def)
{
    for (int i = 0; i < s_cfg_n; i++) {
        if (strcmp(s_cfg[i].key, key) == 0) return s_cfg[i].val;
    }
    return def;
}

void hal_cfg_set_i32(const char *key, int32_t value)
{
    for (int i = 0; i < s_cfg_n; i++) {
        if (strcmp(s_cfg[i].key, key) == 0) {
            s_cfg[i].val = value;
            return;
        }
    }
    if (s_cfg_n < 32) {
        snprintf(s_cfg[s_cfg_n].key, sizeof(s_cfg[0].key), "%s", key);
        s_cfg[s_cfg_n++].val = value;
    }
}

void hal_sysinfo(hal_sysinfo_t *out)
{
    out->board          = "TAB5 / SIM";
    out->chip           = "ESP32-P4 rev1.0 x2";
    out->cpu_mhz        = 360;
    out->heap_int_free  = 412 * 1024;
    out->heap_int_total = 592 * 1024;
    out->psram_free     = 27800 * 1024;
    out->psram_total    = 32768 * 1024;
    out->uptime_s       = lv_tick_get() / 1000;
}
