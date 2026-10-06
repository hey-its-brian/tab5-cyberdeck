idf_component_register(
    SRCS "player.c" "id3.c" "aout_tab5.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES deck_hal
    PRIV_REQUIRES minimp3 esp_driver_i2s esp_driver_i2c esp_timer espressif__esp_codec_dev
)
