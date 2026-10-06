/*
 * deck_hal for the desktop simulator: plausible fake hardware so every
 * screen renders as it would on the Tab5.
 */
#include "deck_hal.h"
#include "deck_tz.h"

#include <stdlib.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

/* The sim's "SD card" is a folder; make sure it exists before anyone writes. */
__attribute__((constructor)) static void sim_storage_init(void) { mkdir(DECK_SIM_SDCARD, 0755); }
bool hal_lvgl_lock(uint32_t timeout_ms) { return true; }
void hal_lvgl_unlock(void) {}

void hal_backlight_set(uint8_t percent) { s_backlight = percent; }
void hal_display_power(bool on) { printf("[sim] display %s\n", on ? "on" : "off"); }
void hal_kbd_light(uint8_t b, bool c, uint32_t rgb) { printf("[sim] kbd light %u %s %06x\n", b, c ? "theme" : "status", (unsigned)rgb); }

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

static int s_tz = DECK_TZ_DEFAULT;
int hal_tz_count(void) { return DECK_TZ_COUNT; }
const char *hal_tz_name(int index) { return (index >= 0 && index < DECK_TZ_COUNT) ? g_deck_tz[index].name : "?"; }
int hal_tz_get(void) { return s_tz; }
void hal_tz_set(int index)
{
    s_tz = (index >= 0 && index < DECK_TZ_COUNT) ? index : DECK_TZ_DEFAULT;
    setenv("TZ", g_deck_tz[s_tz].posix, 1);
    tzset();
}

static struct {
    char key[16];
    char val[96];
} s_str[16];

bool hal_cfg_get_str(const char *key, char *out, size_t n)
{
    for (int i = 0; i < 16; i++) {
        if (s_str[i].key[0] && strcmp(s_str[i].key, key) == 0) {
            snprintf(out, n, "%s", s_str[i].val);
            return true;
        }
    }
    if (n) out[0] = '\0';
    return false;
}

void hal_cfg_set_str(const char *key, const char *value)
{
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (s_str[i].key[0] && strcmp(s_str[i].key, key) == 0) {
            if (value) {
                snprintf(s_str[i].val, sizeof(s_str[i].val), "%s", value);
            } else {
                s_str[i].key[0] = '\0';
            }
            return;
        }
        if (!s_str[i].key[0] && free_slot < 0) free_slot = i;
    }
    if (value && free_slot >= 0) {
        snprintf(s_str[free_slot].key, sizeof(s_str[0].key), "%s", key);
        snprintf(s_str[free_slot].val, sizeof(s_str[0].val), "%s", value);
    }
}

/* One blob slot is enough for the simulator (nothing uses more). */
static char s_blob_key[16];
static unsigned char s_blob[4096];
static size_t s_blob_len;

bool hal_cfg_get_blob(const char *key, void *out, size_t *len)
{
    if (!s_blob_key[0] || strcmp(s_blob_key, key) != 0) return false;
    if (out) {
        if (*len < s_blob_len) return false;
        memcpy(out, s_blob, s_blob_len);
    }
    *len = s_blob_len;
    return true;
}

void hal_cfg_set_blob(const char *key, const void *data, size_t len)
{
    if (data == NULL || len > sizeof(s_blob)) {
        if (strcmp(s_blob_key, key) == 0) s_blob_key[0] = '\0';
        return;
    }
    snprintf(s_blob_key, sizeof(s_blob_key), "%s", key);
    memcpy(s_blob, data, len);
    s_blob_len = len;
}

bool hal_cfg_encrypted(void) { return false; }

void hal_speaker_amp(bool on) { (void)on; }
bool hal_headphones(void) { return getenv("DECK_SIM_HEADPHONES") != NULL; }

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
