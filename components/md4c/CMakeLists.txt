# md4c 0.5.2 (https://github.com/mity/md4c), vendored unmodified. MIT.
idf_component_register(SRCS "md4c.c" INCLUDE_DIRS ".")
target_compile_definitions(${COMPONENT_LIB} PRIVATE MD4C_USE_UTF8)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-implicit-fallthrough -Wno-maybe-uninitialized)
