# Desktop simulator: the same UI code (deck_ui, deck_core, apps) on SDL2,
# with a fake hardware layer (hal_sim.c). Also renders headless screenshots.
#
#   cmake -S sim -B sim/build && cmake --build sim/build -j
#   sim/build/deck_sim                      # window: mouse = touch, keyboard = Tab5 keyboard
#   sim/build/deck_sim --headless --shot 2500:shots/home.ppm
cmake_minimum_required(VERSION 3.16)
project(deck_sim C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Reuse the LVGL the firmware build downloaded (same version), otherwise fetch it.
set(LVGL_DIR "${ROOT}/managed_components/lvgl__lvgl" CACHE PATH "LVGL source tree")
if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    include(FetchContent)
    FetchContent_Declare(lvgl GIT_REPOSITORY https://github.com/lvgl/lvgl.git GIT_TAG v9.5.0 GIT_SHALLOW TRUE)
    FetchContent_Populate(lvgl)
    set(LVGL_DIR "${lvgl_SOURCE_DIR}")
endif()

find_package(SDL2 REQUIRED)

# cJSON comes from ESP-IDF (components/json) so the sim parses exactly like the device.
set(IDF_DIR "$ENV{IDF_PATH}")
if(NOT IDF_DIR)
    set(IDF_DIR "$ENV{HOME}/esp/esp-idf")
endif()
set(CJSON_DIR "${IDF_DIR}/components/json/cJSON" CACHE PATH "cJSON source")
add_library(cjson STATIC ${CJSON_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON_DIR})

file(GLOB_RECURSE LVGL_SRCS "${LVGL_DIR}/src/*.c")
add_library(lvgl STATIC ${LVGL_SRCS})
target_include_directories(lvgl PUBLIC "${LVGL_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(lvgl PUBLIC SDL2::SDL2)
target_compile_options(lvgl PRIVATE -w)

file(GLOB APP_SRCS "${ROOT}/apps/*/*.c")
set_source_files_properties(${ROOT}/components/md4c/md4c.c PROPERTIES COMPILE_OPTIONS "-w;-DMD4C_USE_UTF8")
file(GLOB APP_DIRS LIST_DIRECTORIES true "${ROOT}/apps/*")

add_executable(deck_sim
    main.c
    hal_sim.c
    net_sim.c
    ota_sim.c
    ${ROOT}/components/deck_ota/ota_version.c
    assets_sim.c
    ${ROOT}/components/deck_ui/deck_theme.c
    ${ROOT}/components/deck_ui/deck_widgets.c
    ${ROOT}/components/deck_ui/deck_fx.c
    ${ROOT}/components/deck_core/deck_app.c
    ${ROOT}/components/deck_core/deck_input.c
    ${ROOT}/components/deck_core/deck_shell.c
    ${ROOT}/components/deck_core/deck_statusbar.c
    ${ROOT}/components/deck_core/deck_launcher.c
    ${ROOT}/components/deck_core/deck_boot.c
    ${ROOT}/components/deck_core/deck_stub.c
    ${ROOT}/components/deck_core/deck_modal.c
    ${ROOT}/components/deck_md/deck_md.c
    ${ROOT}/components/md4c/md4c.c
    ${APP_SRCS}
)
target_include_directories(deck_sim PRIVATE
    ${ROOT}/components/deck_hal/include
    ${ROOT}/components/deck_ui/include
    ${ROOT}/components/deck_core/include
    ${ROOT}/components/deck_md/include
    ${ROOT}/components/deck_net/include
    ${ROOT}/components/deck_ota/include
    ${ROOT}/components/md4c
    ${APP_DIRS}
)
target_compile_definitions(deck_sim PRIVATE
    DECK_ASSET_DIR="${ROOT}/assets/fonts"
    DECK_SIM_SDCARD="${CMAKE_CURRENT_SOURCE_DIR}/sdcard")
target_compile_options(deck_sim PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(deck_sim PRIVATE lvgl cjson)

# Unit tests for the calculator engine (no LVGL).
add_executable(calc_test calc_test.c ${ROOT}/apps/app_calc/calc_engine.c)
target_include_directories(calc_test PRIVATE ${ROOT}/apps/app_calc)
target_compile_options(calc_test PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME calc_engine COMMAND calc_test)

add_executable(weather_test weather_test.c ${ROOT}/apps/app_weather/weather_data.c)
target_include_directories(weather_test PRIVATE ${ROOT}/apps/app_weather ${ROOT}/components/deck_ui/include)
target_compile_definitions(weather_test PRIVATE FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(weather_test PRIVATE cjson m)
add_test(NAME weather_data COMMAND weather_test)
