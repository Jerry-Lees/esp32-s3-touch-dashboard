# A game for the dashboard (SD card app). Build it with: tools/build_app.sh apps/checkers
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/dashboard_app.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(checkers)
include(elf_loader)
project_elf(checkers)
