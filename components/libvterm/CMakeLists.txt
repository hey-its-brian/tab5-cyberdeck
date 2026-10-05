# libvterm 0.3.3 (https://www.leonerd.org.uk/code/libvterm/), vendored unmodified. MIT.
idf_component_register(
    SRCS "src/encoding.c" "src/keyboard.c" "src/mouse.c" "src/parser.c" "src/pen.c"
         "src/screen.c" "src/state.c" "src/unicode.c" "src/vterm.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src")
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
