idf_component_register(
    SRCS "app_weather.c" "weather_data.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_hal deck_net
    PRIV_REQUIRES json
)
