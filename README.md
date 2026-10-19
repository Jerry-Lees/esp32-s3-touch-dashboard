# ESP32-S3 Touch Display

A home dashboard and games console for a 4.3" ESP32-S3 touch screen, written in C with
**ESP-IDF 5.5** and **LVGL 8.4**. It has a launcher with app tiles, a world clock, weather,
a stock watchlist, five games and a settings app. A scrolling info ticker along the bottom of the
home screen keeps the weather and stocks up to date in the background.

![Home screen with the info ticker](docs/screenshots/home.png)

All screenshots were taken on the real device (see [Screenshots](#taking-screenshots)).

## Hardware

**[Waveshare ESP32-S3-Touch-LCD-4.3B-BOX](https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B)**
(SKU 28141): the ESP32-S3-Touch-LCD-4.3B board, already mounted in Waveshare's plastic case.
Nothing else is attached; it's powered and programmed over its USB-C port.

| Part | Details |
|---|---|
| Module | ESP32-S3-WROOM-1-N16R8 (dual-core Xtensa LX7, 240 MHz, Wi-Fi 2.4 GHz + Bluetooth LE), chip revision v0.2 |
| Memory | **16 MB** flash, **8 MB** octal PSRAM |
| Display | 4.3" IPS, **800 x 480**, 16-bit RGB parallel interface (ST7701) |
| Touch | **GT911** capacitive touch (I2C: SDA GPIO8, SCL GPIO9, INT GPIO4) |
| I/O expander | **CH422G** on the same I2C bus: touch reset, LCD backlight (on/off only, no dimming), SD card chip select, 2 isolated digital inputs and 2 open-drain outputs |
| Clock | **PCF85063** real-time clock on I2C. A backup battery can be fitted but isn't in this unit, so it keeps time across resets and reflashes but not power loss |
| Also on the board (not used yet) | microSD slot (SPI), RS485, CAN, digital in/out terminals |
| USB | USB-C to the ESP32-S3's built-in USB-Serial/JTAG (shows up as `/dev/ttyACM0` on Linux) |

Board documentation and schematic:
[Waveshare wiki](https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B),
[schematic PDF](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B/ESP32-S3-Touch-LCD-4.3B-Sch.pdf).

## Features

### Home screen and launcher

- Tile grid of apps, with a clock and Wi-Fi status in the top bar. Games live in a **Games folder**.
- **Info ticker** (optional, Settings > Display): a strip along the bottom fifth of the home
  screen that scrolls the weather for the cities you pick (now, today and tomorrow) and your
  stock watchlist (price, today's change, next earnings call). Its data refreshes in the
  background even when the Weather and St0nks apps are closed.
- **Screen timeout**: the backlight turns off after a chosen time without touches; the tap that
  wakes it doesn't press anything.
- Optional FPS / CPU meter.

### World Clock

![World Clock](docs/screenshots/world_clock.png)

Four cards: US Central (local), Eastern, London and UTC, with the date and UTC offset. Time comes
from the internet (NTP) and is saved to the board's RTC chip, so it's correct straight after a
reset even before Wi-Fi connects. 12- or 24-hour format.

### Weather

![Weather](docs/screenshots/weather.png)

Current conditions, the next 24 hours and a 7-day forecast from [Open-Meteo](https://open-meteo.com/)
(free, no API key). Six saved locations to switch between; refreshes every 15 minutes. The
weather icons are drawn with LVGL shapes, not images.

### St0nks

![St0nks watchlist](docs/screenshots/stonks_list.png)

![St0nks detail](docs/screenshots/stonks_detail.png)

A stock watchlist named after the meme, with a diamond-hands icon.

- **List:** each symbol's price, today's change, a sparkline of the day and the date of the next
  earnings call (or the last one, if the next isn't announced yet).
- **Detail:** tap a stock for a large chart with 1D / 5D / 1M / 6M / 1Y / 5Y ranges (touch and drag
  on the chart to read the price at any point), previous close, day and 52-week high/low,
  volume, exchange and the earnings call date.
- **Add** symbols with the on-screen keyboard (stocks, ETFs, indexes such as `^GSPC`, crypto such as
  `BTC-USD`, futures such as `GC=F`), and remove them from the detail view. The list (up to 20)
  is saved in flash.
- Prices from Yahoo Finance's public chart API (delayed, no key), refreshed every 15 minutes or
  with the refresh button. Earnings dates from Nasdaq's public API (US company stocks only).

### Games

![Games folder](docs/screenshots/games.png)

| | |
|---|---|
| ![Chess](docs/screenshots/chess.png) | ![Checkers](docs/screenshots/checkers.png) |
| **Chess**: full rules (castling, en passant, promotion, draws), alpha-beta search computer opponent with 4 levels, undo. | **Checkers**: American rules with forced captures and multi-jumps, 4 computer levels, undo. |
| ![Flappy](docs/screenshots/flappy.png) | ![Snake](docs/screenshots/snake.png) |
| **Flappy**: tap to flap through the pipes; speeds up and the gaps narrow as the score rises. | **Snake**: swipe to steer; gets faster as it grows. |
| ![2048](docs/screenshots/2048.png) | |
| **2048**: swipe to slide and merge tiles, with slide and pop animations. | |

The computer's chess and checkers moves are worked out in a background task, so the screen
stays responsive. Games keep their state when you leave and come back, and best scores are
saved in flash.

### Settings

| | |
|---|---|
| ![Display settings](docs/screenshots/settings_display.png) | ![Time settings](docs/screenshots/settings_time.png) |
| ![Weather settings](docs/screenshots/settings_weather.png) | ![St0nks settings](docs/screenshots/settings_stonks.png) |
| ![About](docs/screenshots/settings_about.png) | |

Tabs for Network (Wi-Fi status and signal), Display (screen timeout, info ticker, FPS meter),
Time (12/24-hour, time source), Weather (default city, ticker cities), St0nks (show the watchlist
in the ticker) and About (chip, memory, versions). Settings are stored in the ESP32's NVS flash
area and survive reboots and reflashing.

## Building and flashing

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html).
LVGL 8.4 and the touch drivers are included in `components/`.

1. Create `main/wifi_secrets.h` with your Wi-Fi details (2.4 GHz networks only). This file is in
   `.gitignore` so it never gets committed:
   ```c
   #pragma once
   #define WIFI_SSID     "your network name"
   #define WIFI_PASSWORD "your password"
   ```
2. Build and flash (board connected over USB-C):
   ```bash
   . ~/esp/esp-idf/export.sh
   idf.py build
   idf.py -p /dev/ttyACM0 flash
   ```
3. To watch the log output: `idf.py -p /dev/ttyACM0 monitor` (Ctrl+] to exit).

If flashing can't connect, hold **BOOT**, tap **RESET**, release BOOT and try again, then press
RESET once afterwards to start the app.

The flash is split into the settings area (NVS), a 6 MB app partition and a ~9.9 MB `storage`
partition reserved for future use (see `partitions.csv`).

## Taking screenshots

Long-press an empty part of the top bar on any screen: the device sends that screen over the USB
serial port (it pauses for a few seconds meanwhile). On the PC, run the receiver, which saves each
one as a PNG in `docs/screenshots/`:

```bash
~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/screenshot.py
```

(Use the Python that comes with ESP-IDF, which has `pyserial`, and close `idf.py monitor` first.)

## Project layout

| Path | What's there |
|---|---|
| `main/launcher.c` | Home screen, folders, app screens, screen timeout |
| `main/app_*.c` | One file per app |
| `main/chess_engine.c`, `main/checkers_engine.c` | Game engines (plain C, no LVGL; tests in `test/host/` run on a PC) |
| `main/ticker.c` | Home screen info ticker |
| `main/settings.c` | Saved settings (NVS) |
| `main/net_time.c`, `main/rtc.c` | Wi-Fi, NTP time and the RTC chip |
| `main/waveshare_rgb_lcd_port.c`, `main/lvgl_port.c` | Display, touch and LVGL setup, from Waveshare's demo |
| `tools/` | Image generators and the screenshot receiver |
| `CLAUDE.md` | Detailed developer notes |

## Credits

- Started from Waveshare's ESP-IDF LVGL porting demo for this board.
- [LVGL](https://lvgl.io/) 8.4 (MIT licence).
- Weather data: [Open-Meteo](https://open-meteo.com/) (CC BY 4.0).
- Stock prices: Yahoo Finance; earnings dates: Nasdaq. For personal use; data is delayed and not
  investment advice.
- St0nks icon built from [Twemoji](https://github.com/twitter/twemoji) graphics by Twitter (CC BY 4.0).
- Fonts: Montserrat (bundled with LVGL).
