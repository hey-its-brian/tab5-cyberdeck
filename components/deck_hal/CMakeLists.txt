idf_component_register(
    SRCS "hal_tab5.cpp"
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
        esp_timer
)
