idf_component_register(
    SRCS "app_term.c" "term_link_ssh.c" "term_chat_ollama.c"
    INCLUDE_DIRS "."
    REQUIRES deck_core deck_ui deck_hal deck_term
    PRIV_REQUIRES libvterm skuodi__libssh2_esp mbedtls lwip json
)
