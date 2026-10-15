idf_component_register(
    SRCS "ls_hash.c" "ls_crypto.c" "ls_couch.c" "ls_sync.c" "deck_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls json deck_net deck_hal
)
