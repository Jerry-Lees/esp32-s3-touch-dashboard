# An SD card app for the dashboard. Build it with: tools/build_app.sh apps/analog_clock
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/dashboard_app.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(analog_clock)
include(elf_loader)
project_elf(analog_clock)
