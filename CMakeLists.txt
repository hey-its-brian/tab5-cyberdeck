# DECK//OS: cyberdeck firmware for the M5Stack Tab5 + Tab5 Keyboard.
cmake_minimum_required(VERSION 3.16)

# Apps live in their own folder so each one is an isolated component.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/apps")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_cyberdeck)
