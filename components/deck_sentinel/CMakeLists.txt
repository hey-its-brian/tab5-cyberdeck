idf_component_register(
    SRCS "sentinel_tab5.c" "sentinel_store.c"
    INCLUDE_DIRS "include"
    REQUIRES sentinel_core
    PRIV_REQUIRES deck_hal esp_driver_uart esp_driver_gpio esp_timer
)
