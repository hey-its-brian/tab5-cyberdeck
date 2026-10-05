idf_component_register(
    SRCS "deck_md.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES md4c deck_ui
)
