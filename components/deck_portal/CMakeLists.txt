idf_component_register(
    SRCS "portal.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES deck_hal deck_net esp_http_server esp_timer json
    EMBED_TXTFILES "www/index.html"
)
