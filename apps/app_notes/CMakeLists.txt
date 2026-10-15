idf_component_register(
    SRCS "app_notes.c" "notes_store.c"
    INCLUDE_DIRS "."
    REQUIRES deck_sync deck_core deck_ui deck_hal deck_md
)
