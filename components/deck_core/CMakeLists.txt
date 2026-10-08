idf_component_register(
    SRCS
        "deck_app.c"
        "deck_input.c"
        "deck_shell.c"
        "deck_update.c"
        "deck_saver.c"
        "deck_statusbar.c"
        "deck_launcher.c"
        "deck_boot.c"
        "deck_stub.c"
        "deck_modal.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl deck_hal deck_ui deck_net deck_ota deck_portal deck_audio
)
