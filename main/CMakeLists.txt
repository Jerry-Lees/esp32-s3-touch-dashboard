# Built-in app signing keys: every keys/*.pub, gathered into one embedded
# file as "name: <file name>" + the key's PEM text (read by app_keys.c).
set(BUILTIN_KEYS_TXT "${CMAKE_CURRENT_BINARY_DIR}/builtin_keys.txt")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(keys_dir "${CMAKE_CURRENT_LIST_DIR}/../keys")
    file(GLOB BUILTIN_KEYS "${keys_dir}/*.pub")
    list(SORT BUILTIN_KEYS)
    set(keys_text "")
    foreach(k ${BUILTIN_KEYS})
        get_filename_component(kname "${k}" NAME_WE)
        file(READ "${k}" kpem)
        string(APPEND keys_text "name: ${kname}\n${kpem}\n")
    endforeach()
    file(WRITE "${BUILTIN_KEYS_TXT}" "${keys_text}")
    # Re-run when a key changes or one is added/removed (the folder changes).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${keys_dir}" ${BUILTIN_KEYS})
endif()

idf_component_register(
    SRCS "waveshare_rgb_lcd_port.c" "main.c" "lvgl_port.c" "launcher.c" "app_world_clock.c" "net_time.c" "app_weather.c" "settings.c" "rtc.c" "game_common.c" "game_images.c" "chess_engine.c" "app_chess.c" "checkers_engine.c" "app_checkers.c" "app_flappy.c" "app_snake.c" "app_2048.c" "app_stonks.c" "ticker.c" "screenshot.c" "sd_card.c" "wifi_list.c" "settings_backup.c" "photo_frame.c" "pic_decode.c" "app_photos.c" "app_files.c" "usb_files.c" "clock_tools.c" "app_timers.c" "jellyfin.c" "app_jellyfin.c" "json_skip.c" "app_keys.c" "ext_apps.c" "app_symbols.c" "elf_icon.c" "stonks_icon.c" "app_settings.c"
    INCLUDE_DIRS "." "../sdk/dashboard_sdk/include"
    EMBED_TXTFILES "${BUILTIN_KEYS_TXT}")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
