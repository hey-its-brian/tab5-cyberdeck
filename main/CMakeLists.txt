idf_component_register(
    SRCS "main.c"
    REQUIRES deck_net deck_ota deck_hal deck_ui deck_core app_term app_notes app_calc app_weather app_portal app_sys
)
