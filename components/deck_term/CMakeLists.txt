idf_component_register(
    SRCS "deck_term.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl deck_core
    PRIV_REQUIRES libvterm deck_ui deck_hal
)
