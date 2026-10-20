# Build settings shared by all SD card apps. An app's CMakeLists.txt includes
# this before project() (see apps/README.md).
get_filename_component(DASH_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# The firmware's components (LVGL, the ELF loader) and the app SDK.
set(EXTRA_COMPONENT_DIRS "${DASH_ROOT}/components" "${DASH_ROOT}/sdk")
# The firmware's configuration, so LVGL's settings match exactly, plus a few
# app-only changes. The app's own sdkconfig lives in its build folder.
set(SDKCONFIG_DEFAULTS "${DASH_ROOT}/sdkconfig;${DASH_ROOT}/sdk/sdkconfig.app")
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(COMPONENTS main espressif__elf_loader)
