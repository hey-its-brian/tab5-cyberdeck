idf_component_register(
    SRCS "minimp3.c"
    INCLUDE_DIRS "."
)
# minimp3 is upstream code; keep its warnings out of -Werror.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable -O2)
