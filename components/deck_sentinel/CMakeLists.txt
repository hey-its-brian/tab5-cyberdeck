idf_component_register(
    SRCS "sentinel_proto.c" "sentinel_tab5.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES deck_hal esp_driver_uart esp_driver_gpio esp_timer
)
