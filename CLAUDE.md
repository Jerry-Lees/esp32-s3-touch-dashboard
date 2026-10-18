# CLAUDE.md: ESP32-S3 Touch Display Project

Context handed over from an earlier chat session. Read this fully before making changes.

## About the user and setup

- The user is a hobbyist, comfortable in a terminal and new to ESP32/ESP-IDF. Explain what you're doing in plain terms and give exact commands.
- The host PC runs **Bazzite** (immutable Fedora Atomic). Home is `/var/home/dad` (also `~`). Don't layer packages with rpm-ostree or use containers. The user chose a native install, and everything already works.
- There's an unrelated Python venv the user sometimes has active. Run `deactivate` first if `(.venv)` is in the prompt before ESP-IDF work.

## Hardware

**Waveshare ESP32-S3-Touch-LCD-4.3B-BOX**, the 4.3B board in a plastic case (SKU 28141).

- ESP32-S3-WROOM-1-N16R8: **16 MB flash, 8 MB octal PSRAM**. Revision v0.2.
- 4.3" **800×480** RGB IPS LCD. **GT911** capacitive touch on I2C (SDA GPIO8, SCL GPIO9, INT GPIO4).
- **CH422G I/O expander** on the same I2C bus. It drives the touch reset (EXIO1), the LCD backlight enable (EXIO2, **on/off only, no PWM dimming**), and the SD card CS (EXIO4). It also handles the isolated digital inputs (DI0 = EXIO0, DI1 = EXIO5) and the open-drain outputs (DO0 = OD0, DO1 = OD1).
- Other hardware: PCF85063 RTC on I2C; RS485 on GPIO43/44; CAN (TWAI) on GPIO15/16; TF card on SPI (GPIO11/12/13).
- USB-C uses the ESP32-S3's native USB-Serial/JTAG and shows up as **/dev/ttyACM0** (USB ID 303a:1001).
- Docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B
- Schematic: https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-Sch.pdf

## Toolchain

- **ESP-IDF v5.5.x** (5.5.6), cloned at `~/esp/esp-idf` from `release/v5.5`. Tools are in `~/.espressif`; CMake and Ninja were installed through `idf_tools.py`.
- ESP-IDF is not on the PATH by default. **Every shell command that uses idf.py must source it in the same command**, because your shell is not interactive and the `get_idf` alias may not exist:
  ```bash
  . ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build
  ```
- A udev rule at `/etc/udev/rules.d/99-esp32.rules` (vendor 303a, MODE 0666) already gives the user access to the serial port.
- Flash with `idf.py -p /dev/ttyACM0 flash`. **Don't run `idf.py monitor` yourself.** It needs an interactive terminal and will hang. Ask the user to run `idf.py -p /dev/ttyACM0 monitor` and paste the output (Ctrl+] exits).
- If flashing can't connect: hold BOOT, tap RESET, release BOOT, then retry. After a flash done that way, press RESET once to run the app.

## Project

- Location: `~/esp/my_display`. It started as a copy of Waveshare's ESP-IDF demo `08_lvgl_Porting`. The original demo package is in `~/esp/waveshare-demo/`.
- Git repo. `.gitignore` excludes `build/`, `managed_components/` and `main/wifi_secrets.h`. Commit after each working milestone.
- **LVGL 8.4.0** is vendored in `components/lvgl__lvgl`. Use the **LVGL v8 API** (`lv_scr_act`, `lv_btn_create`, `lv_obj_clear_flag`, `lv_timer_del`, `lv_disp_get_ver_res`, and so on), not v9.
- Also vendored: `espressif__esp_lcd_touch` and `espressif__esp_lcd_touch_gt911`.

### Files in `main/`

| File | Purpose |
|---|---|
| `main.c` | Waveshare's `app_main`. Initializes the LCD, then calls `launcher_start()` under `lvgl_port_lock()`, in both branches of the `#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911`. |
| `waveshare_rgb_lcd_port.c/.h` | Waveshare's LCD, touch and CH422G init. Uses the **legacy I2C driver** (`driver/i2c.h`), so don't mix in the new `i2c_master` driver on the same port. |
| `lvgl_port.c/.h` | Waveshare's LVGL glue and LVGL task. **`lvgl_port.h` line 21: `#define CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911 1`.** It shipped as `0`, which disabled touch; we changed it to 1. |
| `app_framework.h` | `app_t` struct, shared fonts (`APP_FONT_HUGE/LARGE/MEDIUM`), colors and the `extern` app declarations. |
| `launcher.c/.h` | Home screen with a tile grid, a top bar with the Central time and a Wi-Fi icon, and app screens with a "< Home" button. **The app registry is `s_apps[]` in `launcher.c`.** |
| `net_time.c/.h` | Wi-Fi station connection, SNTP (pool.ntp.org), POSIX TZ strings, `LOCAL_TZ` (US Central). Exposes `net_time_wifi_configured/connected()` and `net_time_is_synced()`. |
| `settings.c/.h` | NVS-backed user settings (12/24h, screen timeout, default weather city). |
| `app_settings.c` | Settings app (tabs: Network, Display, Time, Weather, About). |
| `wifi_secrets.h` | `WIFI_SSID` and `WIFI_PASSWORD`. **Never print, log, commit or overwrite this file.** |
| `app_world_clock.c` | World Clock: four cards for Central (local), Eastern, London and UTC. 12/24-hour comes from `settings_get_24h()`. |
| `app_weather.c` | Weather: Open-Meteo over HTTPS (`esp_http_client` + `esp_crt_bundle`), downloads in a FreeRTOS task, cJSON parsing, icons drawn from LVGL shapes, 6 locations with Lee's Summit MO as the default (`s_locs[]`), 15-minute refresh. |
| `CMakeLists.txt` | **Lists every source file explicitly in `SRCS`.** Add each new `.c` file here. No `REQUIRES`, so all IDF components are available to `main`. |

### App framework

```c
typedef struct {
    const char *name;
    const char *symbol;      // LV_SYMBOL_* shown on the tile
    uint32_t    color_hex;   // tile colour
    void (*create)(lv_obj_t *parent);   // build UI inside parent (800 x 416 area below the 64px bar)
    void (*destroy)(void);              // delete timers / reset state; launcher deletes the objects
    void (*draw_icon)(lv_obj_t *holder, int size); // optional custom tile icon
    bool pin_last;                       // true = always the last tile (Settings)
} app_t;
```

To add an app: write `main/app_x.c` defining `const app_t x_app`, add the `extern` line in `app_framework.h`, add `&x_app` to `s_apps[]` in `launcher.c`, and add the file to `SRCS` in `main/CMakeLists.txt`.

### Rules learned the hard way

- **Thread safety:** LVGL calls may only run in the LVGL task, meaning LVGL event callbacks and `lv_timer` callbacks. `launcher_start()` is called while holding `lvgl_port_lock()`. Wi-Fi/IP events, SNTP and download tasks must **never** call LVGL; they set flags or results that an `lv_timer` polls (see `app_weather.c` `s_fetch_state` and the `__atomic` loads and stores).
- When deleting the screen you're currently inside an event of, use `lv_obj_del_async`.
- Non-clickable decoration objects inside buttons must have `LV_OBJ_FLAG_CLICKABLE` cleared, or they swallow taps.
- Built-in Montserrat fonts include ASCII, the degree sign (`"\xC2\xB0"`), the bullet and the `LV_SYMBOL_*` icons, but **no other non-ASCII characters**.
- Fonts enabled in menuconfig: Montserrat 14 (default), 20, 28 and 48.
- `sdkconfig` settings that matter: flash size **16MB**, **custom partition table `partitions.csv`**: nvs 24K @0x9000 and phy_init 4K @0xF000 (same as the old built-in table), **factory app 6M @0x10000**, **`storage` FAT ~9.9M @0x610000** (not mounted yet; meant for icons, cover art, fonts and config). The app is about 1.4 MB. Don't move or resize nvs, or saved Settings are lost. **Don't run `idf.py set-target`**: it resets sdkconfig. Edit sdkconfig choices carefully: for a Kconfig choice, unset the old option and set the new one.
- `nvs_flash_init()` currently runs inside `net_time_start()` **only if Wi-Fi is configured**. Anything that uses NVS should initialize it early and unconditionally; it's safe to call twice.
- Harmless build and boot warnings, which you can ignore: "esp_blockdev does not contain a CMakeLists.txt", "Could not use git describe" (disappears once commits exist), and the legacy I2C driver warning.

## Settings (done)

- `settings.c/.h`: NVS-backed settings (namespace "settings", one u8 key each): `settings_get/set_24h`, `_screen_timeout_min`, `_weather_city`. `settings_init()` runs first in `launcher_start()` and initializes NVS unconditionally.
- `app_settings.c`: tabview with Network, Display, Time, Weather and About tabs. It styles widgets dark by hand because the LVGL theme is light.
- `app_t.pin_last = true` keeps a tile last: `build_home()` draws non-pinned apps first, then pinned ones.
- The screen timeout lives in `launcher.c` (`screen_timeout_tick`). It turns the backlight off and puts a full-screen catcher on `lv_layer_top()` that swallows the waking tap.

## Workflow

1. Read the relevant files before editing. The code in `main/` is the source of truth.
2. Make the change, add any new `.c` files to `main/CMakeLists.txt`, and build with `. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build`.
3. Fix compile errors yourself. Then flash with `idf.py -p /dev/ttyACM0 flash` and ask the user to test on the device and paste monitor output if needed.
4. After the user confirms it works, commit with a clear message.

## Ideas list (for later)

Timer/stopwatch, alarm clock (using the RTC so it keeps time without Wi-Fi), Home Assistant dashboard, Jellyfin "now playing" (the user runs a Jellyfin server), network/server monitor, relay control using DO0/DO1, input monitor using DI0/DI1, CAN/OBD-II viewer, Modbus over RS485, SD card photo frame, and syncing the PCF85063 RTC from SNTP so the time survives without Wi-Fi.
