idf_component_register(
    SRCS "app_player.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_hal deck_audio
)
