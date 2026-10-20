# Header-only: the API the dashboard firmware offers to SD card apps.
idf_component_register(INCLUDE_DIRS "include" REQUIRES lvgl__lvgl)

# LVGL's bundled demos don't build with -Werror=format (same as the firmware).
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
