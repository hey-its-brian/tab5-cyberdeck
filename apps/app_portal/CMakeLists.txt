idf_component_register(
    SRCS "app_portal.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_hal deck_net deck_portal
)
