idf_component_register(
    SRCS "hal_tab5.cpp" "lv_mem_psram.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES
        m5_tab5_component
        m5_tab5_keyboard_component
        esp_driver_ledc
        esp_driver_sdmmc
        sdmmc
        fatfs
        wear_levelling
        nvs_flash
        nvs_sec_provider
        efuse
        esp_timer
    WHOLE_ARCHIVE # lv_mem_psram.c defines LVGL's allocator hooks; nothing in this library references them
)
