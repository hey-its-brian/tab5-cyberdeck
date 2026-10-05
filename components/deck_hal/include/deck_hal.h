/*
 * deck_hal: the only layer that knows about real hardware.
 *
 * Everything above this (UI, launcher, apps) talks to the board through these
 * functions, so the same UI code runs on the Tab5 and in the desktop
 * simulator (sim/), which provides its own implementation of this header.
 *
 * Threading: hal_init() starts the LVGL task. Any LVGL call made from another
 * task must be wrapped in hal_lvgl_lock()/hal_lvgl_unlock(). LVGL callbacks
 * (events, timers, indev reads) already run with the lock held.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle ----------------------------------------------------------- */

/* Bring up board, display, touch, LVGL, keyboard, RTC, power monitor, SD.
 * Returns false only if the display could not be started. Optional parts that
 * fail (no keyboard, no SD) are reported through the getters below. */
bool hal_init(void);

bool hal_lvgl_lock(uint32_t timeout_ms);   /* 0 = wait forever */
void hal_lvgl_unlock(void);

/* ---- Display ------------------------------------------------------------- */

#define HAL_SCREEN_W 1280
#define HAL_SCREEN_H 720

void hal_backlight_set(uint8_t percent);   /* 0..100 */

/* ---- Keyboard ------------------------------------------------------------ */

/* One key transition from the Tab5 Keyboard in HID mode.
 * `code` is a USB HID usage ID (0x04 = 'a', 0x28 = Enter, ...).
 * `mods` is the HID modifier byte (bit0 LCtrl, bit1 LShift, bit2 LAlt, ...).
 * The keyboard reports releases with code 0, so `code` is 0 when !pressed. */
typedef struct {
    uint8_t code;
    uint8_t mods;
    bool    pressed;
} hal_key_t;

/* Pop one pending key event. Non-blocking; returns false when empty. */
bool hal_key_poll(hal_key_t *out);

bool    hal_kbd_present(void);
uint8_t hal_kbd_fw_version(void);          /* 0 if unknown */

/* ---- Power --------------------------------------------------------------- */

typedef struct {
    bool  valid;       /* false if the power monitor could not be read */
    float volts;       /* pack voltage */
    float amps;        /* + charging, - discharging */
    int   percent;     /* 0..100 estimate, -1 if no battery */
    bool  charging;
} hal_power_t;

void hal_power_read(hal_power_t *out);

/* ---- Clock --------------------------------------------------------------- */

/* System time is seeded from the RTC at boot; this writes it back to both. */
bool hal_rtc_set(const struct tm *local);
bool hal_rtc_present(void);

/* ---- Storage ------------------------------------------------------------- */

#define HAL_SD_MOUNT "/sdcard"

/* Where user files live: the SD card if one is mounted, otherwise a FAT
 * partition in internal flash (about 3.8 MB). NULL if neither is usable.
 * Paths are POSIX; use stdio/dirent on them directly. */
const char *hal_storage_root(void);
bool        hal_storage_is_sd(void);

bool     hal_sd_mounted(void);
uint64_t hal_sd_total_bytes(void);
uint64_t hal_sd_free_bytes(void);

/* ---- Settings (persisted key/value, NVS on device) ---------------------- */

int32_t hal_cfg_get_i32(const char *key, int32_t def);
void    hal_cfg_set_i32(const char *key, int32_t value);

/* ---- System info --------------------------------------------------------- */

typedef struct {
    const char *board;          /* e.g. "TAB5 / ST7123" */
    const char *chip;           /* e.g. "ESP32-P4 rev1.0" */
    uint32_t    cpu_mhz;
    size_t      heap_int_free;
    size_t      heap_int_total;
    size_t      psram_free;
    size_t      psram_total;
    uint32_t    uptime_s;
} hal_sysinfo_t;

void hal_sysinfo(hal_sysinfo_t *out);

#ifdef __cplusplus
}
#endif
