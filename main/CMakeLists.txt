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

# Always built in.
set(srcs "waveshare_rgb_lcd_port.c" "main.c" "lvgl_port.c" "launcher.c" "net_time.c" "settings.c"
         "rtc.c" "game_common.c" "ticker.c" "screenshot.c"
         "sd_card.c" "wifi_list.c" "settings_backup.c" "photo_frame.c" "pic_decode.c" "usb_files.c"
         "clock_tools.c" "json_skip.c" "app_keys.c" "ext_apps.c" "app_symbols.c" "elf_icon.c"
         "app_settings.c")
set(embed "${BUILTIN_KEYS_TXT}")

# Optional apps: switches in menuconfig > "Dashboard apps" (main/Kconfig.projbuild).
if(CONFIG_DASH_APP_WORLD_CLOCK)
    list(APPEND srcs "app_world_clock.c")
endif()
if(CONFIG_DASH_APP_TIMERS)
    list(APPEND srcs "app_timers.c")
endif()
if(CONFIG_DASH_APP_WEATHER)
    list(APPEND srcs "app_weather.c")
endif()
if(CONFIG_DASH_APP_STONKS)
    list(APPEND srcs "app_stonks.c" "stonks_icon.c")
endif()
if(CONFIG_DASH_APP_PHOTOS)
    list(APPEND srcs "app_photos.c")
endif()
if(CONFIG_DASH_APP_FILES)
    list(APPEND srcs "app_files.c")
endif()
if(CONFIG_DASH_APP_JELLYFIN)
    list(APPEND srcs "jellyfin.c" "app_jellyfin.c")
endif()
if(CONFIG_DASH_APP_DASHPAD)
    list(APPEND srcs "dashpad.c" "app_dashpad.c" "usb_kbd.c")
    list(APPEND embed "../dashpad/dashpad.json")
endif()
if(CONFIG_DASH_APP_SERVERS)
    list(APPEND srcs "monitor.c" "app_monitor.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "../sdk/dashboard_sdk/include"
    EMBED_TXTFILES ${embed})

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
