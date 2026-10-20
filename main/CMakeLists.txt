idf_component_register(
    SRCS "waveshare_rgb_lcd_port.c" "main.c" "lvgl_port.c" "launcher.c" "app_world_clock.c" "net_time.c" "app_weather.c" "settings.c" "rtc.c" "game_common.c" "game_images.c" "chess_engine.c" "app_chess.c" "checkers_engine.c" "app_checkers.c" "app_flappy.c" "app_snake.c" "app_2048.c" "app_stonks.c" "ticker.c" "screenshot.c" "sd_card.c" "wifi_list.c" "settings_backup.c" "photo_frame.c" "pic_decode.c" "app_photos.c" "app_files.c" "usb_files.c" "clock_tools.c" "app_timers.c" "jellyfin.c" "app_jellyfin.c" "json_skip.c" "app_keys.c" "ext_apps.c" "app_symbols.c" "elf_icon.c" "stonks_icon.c" "app_settings.c"
    INCLUDE_DIRS "." "../sdk/dashboard_sdk/include"
    EMBED_TXTFILES "../keys/builtin_app_key.pub")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
