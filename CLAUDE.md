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
- **Public GitHub remote:** `origin` = https://github.com/Jerry-Lees/esp32-s3-touch-dashboard (branch `main`). **Never `git push` (or anything else that publishes to GitHub: `gh repo`/`gh release`, force-push, changing visibility) unless the user expressly says to push, asks for it, or confirms when asked.** "Commit it" means a local commit only. Approval to push covers that one push, not later ones.
- **LVGL 8.4.0** is vendored in `components/lvgl__lvgl`. Use the **LVGL v8 API** (`lv_scr_act`, `lv_btn_create`, `lv_obj_clear_flag`, `lv_timer_del`, `lv_disp_get_ver_res`, and so on), not v9.
- Also vendored: `espressif__esp_lcd_touch` and `espressif__esp_lcd_touch_gt911`.

### Files in `main/`

| File | Purpose |
|---|---|
| `main.c` | Waveshare's `app_main`. Initializes the LCD, then calls `launcher_start()` under `lvgl_port_lock()`, in both branches of the `#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911`. |
| `waveshare_rgb_lcd_port.c/.h` | Waveshare's LCD, touch and CH422G init. Uses the **legacy I2C driver** (`driver/i2c.h`), so don't mix in the new `i2c_master` driver on the same port. |
| `lvgl_port.c/.h` | Waveshare's LVGL glue and LVGL task. **`lvgl_port.h` line 21: `#define CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_GT911 1`.** It shipped as `0`, which disabled touch; we changed it to 1. |
| `app_framework.h` | `app_t` struct, shared fonts (`APP_FONT_HUGE/LARGE/MEDIUM`), colors and the `extern` app declarations. |
| `launcher.c/.h` | `launcher_toast(text, ms)`: a message banner on `lv_layer_top()` (tap to dismiss). Home screen with a tile grid, a top bar with the Central time and a Wi-Fi icon, and app screens with a back button. Supports **folders** (a tile that opens a second grid; back from an app returns to its folder). **The app registry is `s_apps[]` in `launcher.c`; games are listed in `s_games[]` (the "Games" folder).** |
| `net_time.c/.h` | Wi-Fi station connection: scans, then joins the **strongest known network** (saved list + optional built-in `wifi_secrets.h`, which is `__has_include`-optional), pinned to that AP's BSSID/channel; rescans with backoff after a disconnect. All Wi-Fi decisions run in the default event loop task; other tasks post `NET_KICK` via `net_time_networks_changed()`, and `NET_UI_SCAN` via `net_time_scan_request()` (Settings' scan waits until not connecting; results go to a buffer read with `net_time_scan_results()`, not used to connect). The driver's own NVS copy of the credentials is erased once (`esp_wifi_restore`, flag `wifi_rst`) and it uses `WIFI_STORAGE_RAM`. Also SNTP (pool.ntp.org), POSIX TZ strings, `LOCAL_TZ` (US Central). Exposes `net_time_wifi_configured/connected()`, `net_time_is_synced()` (true once the time is real, from either the RTC or the internet), `net_time_source()` and `net_time_last_sync()`. At boot it restores the time from the RTC; every SNTP sync (`sync_cb`) writes the time back to the RTC. |
| `rtc.c/.h` | PCF85063 RTC (I2C 0x51, legacy driver, port 0). Stores **UTC**; `rtc_read_utc()` rejects the time if the chip's oscillator-stop flag is set or the date is implausible. |
| `settings.c/.h` | NVS-backed user settings (12/24h, screen timeout, default weather city, show FPS meter). |
| `app_settings.c` | Settings app (tabs: Network, Display, Time, Weather, St0nks, About). |
| `game_common.c/.h` | Shared game code: 8x8 board widget (custom draw, tap -> row/col), background **worker task** (`game_worker_start/busy`, core 0, prio 1) for computer moves, modal dialog, level selector, buttons, and `game_swipe_layer()` (swipe/tap input for Snake and 2048). |
| `game_sys.h` | `game_time_us/random/worker_yield`: the only system calls the engines use, so they also compile on the PC. |
| `game_images.c` | **Generated** by `tools/gen_game_images.py` (chess pieces, crown, Flappy bird frames; TRUE_COLOR_ALPHA RGB565). Don't edit by hand. |
| `chess_engine.c/.h`, `app_chess.c` | Chess: full rules, alpha-beta search, 4 levels. Level saved as NVS key `chess_lvl`. |
| `checkers_engine.c/.h`, `app_checkers.c` | American checkers, 4 levels. NVS key `checkers_lvl`. |
| `app_flappy.c` | Flappy bird clone: time-based physics in a 16 ms `lv_timer`, pipes speed up and gaps narrow with the score. Best score in NVS key `flappy_best`. |
| `app_snake.c` | Snake: swipe to steer (finger can keep moving), custom-draw board that only invalidates changed cells, speeds up with the score. NVS key `snake_best`. |
| `app_2048.c` | 2048: game state `M` survives closing the app; one LVGL object per tile, slide/pop animations with `lv_anim`. NVS key `2048_best`. |
| `app_stonks.c` | St0nks stock watchlist: Yahoo Finance chart API (`query1.finance.yahoo.com/v8/finance/chart/SYM?range=..&interval=..`, no key, needs a browser User-Agent). Persistent worker task + FreeRTOS job/result queues; list with sparklines, detail chart (1D-5Y, touch to scrub), add via on-screen keyboard, remove. Prices auto-refresh every 15 minutes (the user asked for no faster; the refresh button fetches at once). Earnings-call dates (next scheduled, else last) from Nasdaq's public API (`api.nasdaq.com/api/analyst/SYM/earnings-date` and `/api/company/SYM/earnings-surprise`, no key, US company stocks only: `instrumentType` EQUITY), checked every 12 h, shown next to the symbol in the list and as the last row of the detail stats. Watchlist in NVS key `stonks_list` (comma separated, max 20). |
| `stonks_icon.c` | **Generated** by `tools/gen_stonks_icon.py` (diamond-hands icon from Twemoji, CC-BY 4.0). |
| `ticker.c/.h` | Info ticker: 96 px strip at the bottom of the **home screen only** (when `settings_get_ticker()`). One label per item marching left (keeps each under LVGL's 8191 px coordinate limit); item text is rebuilt as it enters. Sources: `weather_ticker_items` (chosen cities: now + today + tomorrow) and `stonks_ticker_items` (price, change, earnings date), declared in `app_framework.h`. `ticker_service()` (1 s launcher timer) calls `weather_background_tick` / `stonks_background_tick` so data refreshes while those apps are closed. |
| `screenshot.c/.h` | Long-press any launcher top bar -> `lv_snapshot` of the screen. **With the SD card mounted** it's saved as a 24-bit BMP in `SD_DIR/screenshots/shot_YYYYMMDD_HHMMSS.bmp` (numbered if the clock isn't set) plus a toast; **without a card** it's printed to the USB serial console as base64 lines (`SHOT-BEGIN w h n` / `SHOT-i:...` / `SHOT-END`). `tools/screenshot.py` (run with the IDF venv Python, which has pyserial; it's fine for Claude to run it in the background, unlike `idf.py monitor`) saves them as PNGs. README images live in `docs/screenshots/`; **hide private details** (Wi-Fi name, IP, MAC) before committing them. |
| `sd_card.c/.h` | microSD on SPI2 (MOSI 11, CLK 12, MISO 13, CS = CH422G EXIO4, kept low: the backlight writes are 0x0E/0x0A, not Waveshare's 0x1E/0x1A which deselected the card). `sd_card_start()` (from `app_main`, after `launcher_start`) mounts it at `/sdcard` in a background task (boot only, no hot-plug), never formats, creates `SD_DIR` = `/sdcard/dashboard/{import,pictures,screenshots,backup,apps}` + `README.txt`, and never touches anything outside it. **`import/` is a one-time inbox**: files are applied, then erased. `import/wifi.json` (or `wifi.json.txt`): validated all-or-nothing, saved via `wifi_list_add`, then **overwritten with zeros and deleted** (`secure_delete`); on error the file stays and a message explains. Messages reach the screen via `sd_card_take_notice()` -> `launcher_toast()`. Never log file contents (they may hold passwords). **Needs exFAT enabled in ESP-IDF** (below). |
| `wifi_list.c/.h` | Networks are added by the SD import or on screen: Settings > Network > **Add network** (overlay `add` in `app_settings.c`: scan list from `net_time_scan_request/results`, then a password page with `lv_keyboard`; WEP/Enterprise shown but disabled). Saved Wi-Fi networks, **max 10** (`WIFI_LIST_MAX`), NVS namespace `wifi` key `nets` (blob of `wifi_net_t`), mutex-protected, RAM copies zeroed after use. Passwords only go to `net_time.c`. Removal in Settings > Network (`wifi_list_remove` + `net_time_networks_changed`) only marks NVS entries erased (old bytes stay in flash). **`wifi_list_wipe()`** (Settings > Network, "Securely wipe all saved networks") copies every NVS value except namespaces `wifi` and `nvs.net80211` to PSRAM, `nvs_flash_deinit/erase/init`, and writes them back; verified on device with an NVS dump (`esptool read_flash 0x9000 0x6000`; count strings with Python's `bytes.count`, **not grep**, which missed keys in the binary dump). |
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
    const struct app_s *const *folder;   // non-NULL = folder tile: NULL-terminated list of apps
} app_t;
```

To add an app: write `main/app_x.c` defining `const app_t x_app`, add the `extern` line in `app_framework.h`, add `&x_app` to `s_apps[]` (or `s_games[]` for a game) in `launcher.c`, and add the file to `SRCS` in `main/CMakeLists.txt`.

### Rules learned the hard way

- **Thread safety:** LVGL calls may only run in the LVGL task, meaning LVGL event callbacks and `lv_timer` callbacks. `launcher_start()` is called while holding `lvgl_port_lock()`. Wi-Fi/IP events, SNTP and download tasks must **never** call LVGL; they set flags or results that an `lv_timer` polls (see `app_weather.c` `s_fetch_state` and the `__atomic` loads and stores).
- When deleting the screen you're currently inside an event of, use `lv_obj_del_async`.
- **Don't use `lv_anim_set_custom_exec_cb`** in LVGL 8: it overwrites the anim's `var` with the (stack) `lv_anim_t` itself, which crashed 2048. Use `lv_anim_set_var(obj)` + a normal `exec_cb(void *var, int32_t v)`.
- Non-clickable decoration objects inside buttons must have `LV_OBJ_FLAG_CLICKABLE` cleared, or they swallow taps.
- Built-in Montserrat fonts include ASCII, the degree sign (`"\xC2\xB0"`), the bullet and the `LV_SYMBOL_*` icons, but **no other non-ASCII characters**.
- Fonts enabled in menuconfig: Montserrat 14 (default), 20, 28 and 48.
- `sdkconfig` settings that matter: flash size **16MB**, **custom partition table `partitions.csv`**: nvs 24K @0x9000 and phy_init 4K @0xF000 (same as the old built-in table), **factory app 6M @0x10000**, **`storage` FAT ~9.9M @0x610000** (not mounted yet; meant for icons, cover art, fonts and config). The app is about 1.4 MB. Don't move or resize nvs, or saved Settings are lost. **Don't run `idf.py set-target`**: it resets sdkconfig. Edit sdkconfig choices carefully: for a Kconfig choice, unset the old option and set the new one.
- NVS is initialized by `settings_init()`, first thing in `launcher_start()`, then `wifi_list_init()`, then `net_time_start()`.
- **The RTC has no backup battery** (the user's board has none fitted; the user may add one later). The time survives RESET, reflashing and crashes, but not unplugging. After a power loss the RTC is ignored until Wi-Fi syncs.
- **cJSON allocates in PSRAM**: St0nks' worker calls `cJSON_InitHooks` (PSRAM malloc, normal `free`) the first time St0nks opens. This applies to every cJSON user (Weather too); it's safe because `free()` handles both memory types.
- **exFAT is enabled by editing ESP-IDF itself** (the user's choice, documented in README "exFAT support"): `~/esp/esp-idf/components/fatfs/src/ffconf.h` has `#define FF_FS_EXFAT 1` (ships as 0, no Kconfig). `sd_card.c` has an `#error` if it's off. After any ESP-IDF update or reinstall, redo the edit. With exFAT on, ESP-IDF 5.5's `ff.c` only compiles with `CONFIG_FATFS_USE_LABEL=y` (set in sdkconfig). Long filenames are on (`CONFIG_FATFS_LFN_HEAP`).
- Reading the boot log yourself: a small pyserial script (IDF venv Python) that opens `/dev/ttyACM0` with DTR/RTS low, pulses RTS to reset, and reads for N seconds works fine non-interactively (unlike `idf.py monitor`). The log contains the Wi-Fi name and IP, so don't paste it into anything public.
- Harmless build and boot warnings, which you can ignore: "esp_blockdev does not contain a CMakeLists.txt", "Could not use git describe" (disappears once commits exist), and the legacy I2C driver warning.

## Settings (done)

- `settings.c/.h`: NVS-backed settings (namespace "settings", one u8 key each): `settings_get/set_24h`, `_screen_timeout_min`, `_weather_city`. Generic `settings_get/set_u8`, `settings_get/set_i32(key, default)` and `settings_get/set_str` for apps' own values. `settings_init()` runs first in `launcher_start()` and initializes NVS unconditionally.
- `app_settings.c`: tabview with Network, Display, Time, Weather and About tabs. It styles widgets dark by hand because the LVGL theme is light.
- `app_t.pin_last = true` keeps a tile last: `build_grid()` draws non-pinned apps first, then pinned ones.
- **FPS meter:** `CONFIG_LV_USE_PERF_MONITOR` stays enabled; its label on `lv_layer_sys()` is hidden or shown by `launcher_apply_fps()` according to `settings_get_show_fps()` (key `show_fps`, default off; checkbox in Settings > Display).
- **Info ticker settings:** `ticker` (Display tab, default off), `tk_wx` + `tk_wxcity` bit mask (Weather tab; 0xFF = unset = default city), `tk_stk` (St0nks tab). With the ticker on, `layout_home()` uses compact tiles (140x132, 5 per row) above the strip; `launcher_apply_ticker()` rebuilds it.
- The screen timeout lives in `launcher.c` (`screen_timeout_tick`). It turns the backlight off and puts a full-screen catcher on `lv_layer_top()` that swallows the waking tap.

## Games notes

- Engines contain no LVGL and no ESP code (only `game_sys.h`), so they are tested on the PC: `gcc -O2 -I main -o /tmp/chess_test test/host/chess_test.c main/chess_engine.c && /tmp/chess_test` (same pattern for `checkers_test.c`). Perft must match; run these after any engine change. The checkers test plays Expert games, so it takes several minutes.
- The computer move runs in the worker task. The app's `lv_timer` starts it, polls `game_worker_busy()` and applies the result. A generation counter (`s_gen`) discards stale results after Undo, New game or leaving the app. Searches must call `game_worker_yield()` regularly (via `check_stop`) or the core-0 idle watchdog fires.
- Games keep their state in static memory (history in PSRAM), so leaving and reopening an app resumes the game. The state is lost on reboot.

## Workflow

1. Read the relevant files before editing. The code in `main/` is the source of truth.
2. Make the change, add any new `.c` files to `main/CMakeLists.txt`, and build with `. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py build`.
3. Fix compile errors yourself. Then flash with `idf.py -p /dev/ttyACM0 flash` and ask the user to test on the device and paste monitor output if needed.
4. After the user confirms it works, commit with a clear message. **Don't push** unless the user explicitly asks (see Project above).

## Ideas list (for later)

Weather: colour-tinted cards per condition (grey icons on cloudy days look flat). Timer/stopwatch, alarm clock (using the RTC so it keeps time without Wi-Fi), Home Assistant dashboard, Jellyfin "now playing" (the user runs a Jellyfin server), network/server monitor, relay control using DO0/DO1, input monitor using DI0/DI1, CAN/OBD-II viewer, Modbus over RS485 and an SD card photo frame (the card already mounts at `/sdcard`).

**Very, very low priority** (the user said so; don't suggest them as next steps):

- **Sound:** the board has no speaker, buzzer or audio chip. Options: a 12/24 V buzzer on an isolated output (DO0/DO1, needs a 7-36 V DC supply), or an I2S amplifier (e.g. MAX98357A), which needs 3 free GPIOs taken from RS485/CAN/SD. Bluetooth speakers are impossible: the ESP32-S3 has BLE only, no Classic/A2DP.
- **Batteries:** a CR927 coin cell in the RTC holder (keeps time when unplugged; RTC sync is already done), and a 3.7 V single-cell LiPo on the MX1.25 connector (with its own switch) to run the device unplugged.
