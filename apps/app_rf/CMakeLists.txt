idf_component_register(
    SRCS "app_rf.c" "rf_overview.c" "rf_log.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_sentinel deck_hal
)
