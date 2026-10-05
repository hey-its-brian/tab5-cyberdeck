set(fonts "${CMAKE_CURRENT_LIST_DIR}/../../assets/fonts")

idf_component_register(
    SRCS "deck_theme.c" "deck_widgets.c" "deck_fx.c" "assets_idf.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl deck_hal
    EMBED_FILES
        "${fonts}/Orbitron.ttf"
        "${fonts}/ShareTechMono-Regular.ttf"
        "${fonts}/icons.ttf"
)
