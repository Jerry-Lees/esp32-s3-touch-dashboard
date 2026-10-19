idf_component_register(
    SRCS "waveshare_rgb_lcd_port.c" "main.c" "lvgl_port.c" "launcher.c" "app_world_clock.c" "net_time.c" "app_weather.c" "settings.c" "rtc.c" "game_common.c" "game_pieces.c" "chess_engine.c" "app_chess.c" "checkers_engine.c" "app_checkers.c" "app_settings.c"
    INCLUDE_DIRS ".")

idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${lvgl_lib} PRIVATE -Wno-format)
