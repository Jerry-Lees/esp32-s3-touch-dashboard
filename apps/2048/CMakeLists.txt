# A game for the dashboard (SD card app). Build it with: tools/build_app.sh apps/2048
cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../../sdk/dashboard_app.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(g2048)
include(elf_loader)
project_elf(g2048)
