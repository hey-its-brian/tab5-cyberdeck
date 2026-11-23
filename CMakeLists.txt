# DECK//OS: cyberdeck firmware for the M5Stack Tab5 + Tab5 Keyboard.
cmake_minimum_required(VERSION 3.16)

# Apps live in their own folder so each one is an isolated component.
# sentinel_core is the portable RF Sentinel library, from the rf-sentinel
# submodule (git submodule update --init). The submodule sits outside
# components/ because its root is an ESP-IDF project of its own.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/apps"
                         "${CMAKE_CURRENT_LIST_DIR}/external/rf-sentinel/sentinel_core")

# Stored in the image header; OTA compares against GitHub release tags.
# Keep in step with DECK_VERSION in components/deck_core/include/deck_shell.h.
set(PROJECT_VER "1.5.0-beta.2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_cyberdeck)
