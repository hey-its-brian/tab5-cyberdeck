/*
 * deck_hal implementation for the M5Stack Tab5 + Tab5 Keyboard.
 *
 * Board bring-up is delegated to M5Stack's m5_tab5_component, which detects
 * the panel revision (ILI9881C / ST7123 / ST7121) at runtime. The panel is
 * natively 720x1280 portrait; we rotate to 1280x720 landscape with the P4's
 * PPA so the keyboard sits below the screen.
 */
#include "deck_hal.h"
#include "deck_tz.h"

#include <stdlib.h>

#include <string.h>
#include <sys/time.h>

#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "lvgl_port.h"
#include "m5_tab5_component.h"
#include "m5_tab5_keyboard.h"

static const char *TAG = "hal";

using m5::tab5::m5tab5_component;

static m5tab5_component s_board;
static m5::M5Tab5Keyboard s_kbd;

static QueueHandle_t s_key_q;
static volatile bool s_kbd_present;
static uint8_t s_kbd_fw;
static bool s_rtc_ok;
static bool s_ina_ok;
static SemaphoreHandle_t s_ina_mutex;
static bool s_sd_ok;
static sdmmc_card_t *s_card;
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static char s_board_name[32] = "TAB5";
static char s_chip_name[32];

/* ---- Display ------------------------------------------------------------- */

/* Mirrors M5Stack's reference init: direct mode (only dirty areas are
 * redrawn), frame buffer in PSRAM, PPA rotation to landscape. */
static bool display_init(void)
{
    esp_lcd_panel_handle_t panel = s_board.lcd_panel();
    if (panel == nullptr) {
        ESP_LOGE(TAG, "no LCD panel handle");
        return false;
    }

    lvgl_port_cfg_t lvgl_cfg   = {};
    lvgl_cfg.task_priority     = 6;
    lvgl_cfg.task_stack        = 24 * 1024;
    lvgl_cfg.task_affinity     = 1;
    lvgl_cfg.task_max_sleep_ms = 500;
    lvgl_cfg.task_stack_caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT;
    lvgl_cfg.timer_period_ms   = 5;
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return false;
    }

    lvgl_disp_cfg_t disp_cfg    = {};
    disp_cfg.panel_handle       = panel;
    disp_cfg.hres               = HAL_SCREEN_H; /* native portrait */
    disp_cfg.vres               = HAL_SCREEN_W;
    disp_cfg.buffer_size        = HAL_SCREEN_W * HAL_SCREEN_H;
    disp_cfg.color_format       = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.full_refresh = 0;
    disp_cfg.flags.direct_mode  = 1;
    disp_cfg.flags.buff_spiram  = 1;
    disp_cfg.flags.sw_rotate    = 1;

    lvgl_disp_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.sw_rotation         = LV_DISPLAY_ROTATION_90;
    dsi_cfg.flags.avoid_tearing = 1;
    dsi_cfg.flags.use_ppa       = 1;

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (disp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return false;
    }

    esp_lcd_touch_handle_t tp = s_board.touch_panel();
    if (tp != nullptr) {
        lvgl_touch_cfg_t touch_cfg = {};
        touch_cfg.disp             = disp;
        touch_cfg.handle           = tp;
        lv_indev_t *touch          = lvgl_port_add_touch(&touch_cfg);
        if (touch != nullptr) {
            lvgl_port_set_touch_rotation(touch, LV_DISPLAY_ROTATION_90);
        } else {
            ESP_LOGW(TAG, "touch indev not added");
        }
    }
    return true;
}

void hal_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent < 3) percent = 3; /* never fully dark: the user could not find the slider */
    /* 12-bit LEDC channel 1, configured by every panel driver in the BSP. */
    uint32_t duty = (4095u * percent) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

bool hal_lvgl_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void hal_lvgl_unlock(void) { lvgl_port_unlock(); }

/* ---- Keyboard ------------------------------------------------------------ */

/* Runs on the keyboard driver's poll task. Only queue the event here; the
 * LVGL task drains the queue from its keypad read callback. */
static void kbd_event_cb(m5_tab5_key_event_t ev, void *arg)
{
    (void)arg;
    if (ev.type != M5_TAB5_KB_MODE_HID) {
        return;
    }
    hal_key_t k = {};
    k.code    = ev.hid_key_code;
    k.mods    = ev.hid_modifier;
    k.pressed = ev.hid_key_code != 0;
    if (xQueueSend(s_key_q, &k, 0) != pdTRUE) {
        ESP_LOGW(TAG, "key queue full, dropped 0x%02x", k.code);
    }
}

static bool kbd_try_attach(void)
{
    m5::M5Tab5Keyboard::setLogLevel(M5_TAB5_KB_LOG_LEVEL_NONE);
    /* Polling, not the INT pin: the INT line is edge triggered and a key that
     * lands between "read queue" and "clear status" would be missed until the
     * next press. A 10 ms poll of one register is cheap and never sticks. */
    m5_tab5_kb_err_t err = s_kbd.begin(I2C_NUM_1, M5_TAB5_KB_DEFAULT_ADDR, M5_TAB5_KB_DEFAULT_SDA,
                                       M5_TAB5_KB_DEFAULT_SCL, M5_TAB5_KB_I2C_FREQ_400K,
                                       M5_TAB5_KB_INT_MODE_POLLING);
    if (err != M5_TAB5_KB_OK) {
        return false;
    }
    s_kbd.setInterruptMode(M5_TAB5_KB_INT_MODE_POLLING, 10);
    if (s_kbd.enableHIDMode(kbd_event_cb, nullptr) != M5_TAB5_KB_OK) {
        s_kbd.end();
        return false;
    }
    s_kbd.clearEventQueue();
    uint8_t ver = 0;
    s_kbd_fw    = (s_kbd.getVersion(&ver) == M5_TAB5_KB_OK) ? ver : 0;
    ESP_LOGI(TAG, "keyboard attached, fw 0x%02x", s_kbd_fw);
    return true;
}

/* Attaches the keyboard when it is plugged in and notices when it is pulled. */
static void kbd_monitor_task(void *arg)
{
    (void)arg;
    int misses = 0;
    for (;;) {
        if (!s_kbd_present) {
            if (kbd_try_attach()) {
                s_kbd_present = true;
                misses        = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        uint8_t ver = 0;
        if (s_kbd.getVersion(&ver) == M5_TAB5_KB_OK) {
            misses = 0;
        } else if (++misses >= 2) {
            ESP_LOGW(TAG, "keyboard detached");
            s_kbd_present = false;
            s_kbd.end();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool hal_key_poll(hal_key_t *out) { return s_key_q && xQueueReceive(s_key_q, out, 0) == pdTRUE; }
bool hal_kbd_present(void) { return s_kbd_present; }
uint8_t hal_kbd_fw_version(void) { return s_kbd_fw; }

/* ---- Power --------------------------------------------------------------- */

/* 2S Li-ion pack (NP-F550 style). Linear estimate between 6.4 V and 8.3 V is
 * crude but honest enough for a status bar. */
static int pack_percent(float v)
{
    if (v < 5.0f) return -1; /* no pack: running from USB */
    float p = (v - 6.4f) / (8.3f - 6.4f) * 100.0f;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return (int)(p + 0.5f);
}

void hal_power_read(hal_power_t *out)
{
    memset(out, 0, sizeof(*out));
    out->percent = -1;
    if (!s_ina_ok) return;
    m5tab5_component::ina226_reading_t r = {};
    xSemaphoreTake(s_ina_mutex, portMAX_DELAY);
    esp_err_t err = s_board.ina226_read(&r);
    xSemaphoreGive(s_ina_mutex);
    if (err != ESP_OK) return;
    out->valid    = true;
    out->volts    = r.bus_voltage_v;
    out->amps     = r.current_a;
    out->percent  = pack_percent(r.bus_voltage_v);
    out->charging = r.current_a > 0.02f;
}

/* ---- Clock --------------------------------------------------------------- */

/* The RTC holds local wall time. With no TZ set, libc treats it as UTC and
 * localtime() hands the same fields back, which is what we want until v0.4
 * adds SNTP and a timezone setting. */
static void rtc_seed_system_time(void)
{
    m5::tab5::m5tab5_rtc_datetime_t dt;
    if (s_board.rtc_get_datetime(&dt) != ESP_OK) return;
    struct tm t = dt.to_tm();
    if (t.tm_year < 124) { /* before 2024: RTC was never set */
        ESP_LOGW(TAG, "RTC not set (%d-%02d-%02d)", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        return;
    }
    struct timeval tv = {mktime(&t), 0};
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "time from RTC: %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min);
}

bool hal_rtc_set(const struct tm *local)
{
    struct tm t = *local;
    time_t epoch = mktime(&t); /* normalizes fields and fills tm_wday */
    struct timeval tv = {epoch, 0};
    settimeofday(&tv, nullptr);
    if (!s_rtc_ok) return false;
    m5::tab5::m5tab5_rtc_datetime_t dt(t);
    return s_board.rtc_set_datetime(&dt) == ESP_OK;
}

bool hal_rtc_present(void) { return s_rtc_ok; }

static int s_tz = DECK_TZ_DEFAULT;

static void tz_apply(int index)
{
    s_tz = (index >= 0 && index < DECK_TZ_COUNT) ? index : DECK_TZ_DEFAULT;
    setenv("TZ", g_deck_tz[s_tz].posix, 1);
    tzset();
}

int hal_tz_count(void) { return DECK_TZ_COUNT; }
const char *hal_tz_name(int index) { return (index >= 0 && index < DECK_TZ_COUNT) ? g_deck_tz[index].name : "?"; }
int hal_tz_get(void) { return s_tz; }

void hal_tz_set(int index)
{
    time_t now = time(nullptr);
    tz_apply(index);
    hal_cfg_set_i32("tz", s_tz);
    struct tm local;
    localtime_r(&now, &local);
    hal_rtc_set(&local); /* same instant, new wall time */
}

/* ---- Storage ------------------------------------------------------------- */

static void sd_mount(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot         = SDMMC_HOST_SLOT_0; /* IOMUX pins 39..44 on the P4 */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    /* Slot 0 IO is powered from on-chip LDO channel 4. */
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {};
    ldo_cfg.ldo_chan_id              = 4;
    sd_pwr_ctrl_handle_t pwr         = nullptr;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr) == ESP_OK) {
        host.pwr_ctrl_handle = pwr;
    } else {
        ESP_LOGW(TAG, "SD LDO not acquired, trying without it");
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width               = 4;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed           = false;
    mcfg.max_files                        = 8;
    mcfg.allocation_unit_size             = 16 * 1024;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(HAL_SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err == ESP_OK) {
        s_sd_ok = true;
        ESP_LOGI(TAG, "SD mounted: %s, %llu MB", s_card->cid.name,
                 (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size >> 20));
    } else {
        ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(err));
    }
}

/* Internal flash fallback so notes work without a card. */
#define FLASH_MOUNT "/flash"
static bool s_flash_ok;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

static void flash_fs_mount(void)
{
    esp_vfs_fat_mount_config_t mcfg = {};
    mcfg.format_if_mount_failed      = true; /* first boot after the v0.2 partition change */
    mcfg.max_files                   = 4;
    mcfg.allocation_unit_size        = 4096;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_MOUNT, "storage", &mcfg, &s_wl);
    s_flash_ok    = err == ESP_OK;
    if (!s_flash_ok) ESP_LOGW(TAG, "internal storage not mounted (%s)", esp_err_to_name(err));
}

const char *hal_storage_root(void)
{
    if (s_sd_ok) return HAL_SD_MOUNT;
    if (s_flash_ok) return FLASH_MOUNT;
    return nullptr;
}

bool hal_storage_is_sd(void) { return s_sd_ok; }

bool hal_sd_mounted(void) { return s_sd_ok; }

uint64_t hal_sd_total_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_sd_ok) esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
    return total;
}

uint64_t hal_sd_free_bytes(void)
{
    uint64_t total = 0, free_b = 0;
    if (s_sd_ok) esp_vfs_fat_info(HAL_SD_MOUNT, &total, &free_b);
    return free_b;
}

/* ---- Settings ------------------------------------------------------------ */

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    s_nvs_ok = (err == ESP_OK) && nvs_open("deck", NVS_READWRITE, &s_nvs) == ESP_OK;
}

int32_t hal_cfg_get_i32(const char *key, int32_t def)
{
    int32_t v = def;
    if (s_nvs_ok && nvs_get_i32(s_nvs, key, &v) != ESP_OK) v = def;
    return v;
}

void hal_cfg_set_i32(const char *key, int32_t value)
{
    if (!s_nvs_ok) return;
    nvs_set_i32(s_nvs, key, value);
    nvs_commit(s_nvs);
}

bool hal_cfg_get_str(const char *key, char *out, size_t n)
{
    if (n) out[0] = '\0';
    size_t len = n;
    return s_nvs_ok && n && nvs_get_str(s_nvs, key, out, &len) == ESP_OK;
}

void hal_cfg_set_str(const char *key, const char *value)
{
    if (!s_nvs_ok) return;
    if (value) {
        nvs_set_str(s_nvs, key, value);
    } else {
        nvs_erase_key(s_nvs, key);
    }
    nvs_commit(s_nvs);
}

/* ---- System info --------------------------------------------------------- */

void hal_sysinfo(hal_sysinfo_t *out)
{
    out->board          = s_board_name;
    out->chip           = s_chip_name;
    out->cpu_mhz        = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    out->heap_int_free  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out->psram_free     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_total    = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    out->uptime_s       = (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---- Init ---------------------------------------------------------------- */

static void name_board(void)
{
    const m5::tab5::m5tab5_variant_descriptor_t *v = s_board.variant();
    const char *panel                              = "UNKNOWN";
    if (v != nullptr) {
        switch (v->variant_id) {
            case m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ILI9881_TOUCH_GT911: panel = "ILI9881C"; break;
            case m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7123_TOUCH_ST7123: panel = "ST7123"; break;
            case m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7121_TOUCH_ST7121: panel = "ST7121"; break;
            default: break;
        }
    }
    snprintf(s_board_name, sizeof(s_board_name), "TAB5 / %s", panel);

    esp_chip_info_t ci;
    esp_chip_info(&ci);
    snprintf(s_chip_name, sizeof(s_chip_name), "ESP32-P4 rev%d.%d x%d", ci.revision / 100, ci.revision % 100,
             ci.cores);
}

bool hal_init(void)
{
    nvs_init();

    if (s_board.begin() != ESP_OK) {
        ESP_LOGE(TAG, "board begin failed");
        return false;
    }
    name_board();
    ESP_LOGI(TAG, "board: %s", s_board_name);

    /* Power the ESP32-C6 radio. deck_net brings up the ESP-Hosted link to it
     * later, from its own task. */
    s_board.wlan_power(true);

    if (!display_init()) {
        return false;
    }
    hal_backlight_set((uint8_t)hal_cfg_get_i32("bright", 80));

    tz_apply((int)hal_cfg_get_i32("tz", DECK_TZ_DEFAULT)); /* before the RTC is read as local time */
    s_rtc_ok = s_board.rtc_init() == ESP_OK;
    if (s_rtc_ok) {
        rtc_seed_system_time();
    } else {
        ESP_LOGW(TAG, "RTC not found");
    }

    s_ina_mutex = xSemaphoreCreateMutex();
    s_ina_ok    = s_board.ina226_init() == ESP_OK;

    sd_mount();
    if (!s_sd_ok) flash_fs_mount();

    s_key_q = xQueueCreate(64, sizeof(hal_key_t));
    xTaskCreatePinnedToCore(kbd_monitor_task, "kbd_mon", 4096, nullptr, 4, nullptr, 0);
    return true;
}
