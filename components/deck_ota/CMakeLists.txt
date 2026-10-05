idf_component_register(
    SRCS "ota.c" "ota_version.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES deck_hal deck_net esp_https_ota esp_http_client app_update esp_timer mbedtls json
)
