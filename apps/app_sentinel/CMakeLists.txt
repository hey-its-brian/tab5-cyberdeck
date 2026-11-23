idf_component_register(
    SRCS "app_sentinel.c" "app_net.c" "sn_frame.c" "sn_spectrum.c" "sn_wifi.c" "sn_ble.c" "sn_net.c" "sn_drive.c" "sn_log.c" "sn_board.c" "sn_offense.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_sentinel deck_hal deck_net
)
