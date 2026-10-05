idf_component_register(
    SRCS "app_calc.c" "calc_engine.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_hal
)
