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
| microSD | Card slot on SPI (MOSI GPIO11, CLK GPIO12, MISO GPIO13; chip select via the CH422G). FAT32 and exFAT cards; used for photos, files, screenshots, backups and Wi-Fi network import (see [SD card](#sd-card)) |
| Also on the board (not used yet) | RS485, CAN, digital in/out terminals |
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
- **Screen timeout**: after a chosen time without touches, the backlight turns off, or (Settings >
  Display, "When the screen times out") a **photo slideshow** starts. The tap that wakes it doesn't
  press anything.
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

### Photos

A slideshow of the pictures in `dashboard/pictures/` on the SD card (and folders inside it, up to
3 deep), cross-fading between them.

| | |
|---|---|
| ![Photos](docs/screenshots/photos.png) | ![A portrait picture, fitted](docs/screenshots/photos_portrait.png) |

- **Formats**: JPEG, PNG, GIF (first frame) and BMP (24/32-bit). Not supported: progressive JPEG,
  interlaced PNG, HEIC and WebP (convert them to JPEG). Unsupported or damaged files are skipped.
- **Any size**: pictures are shrunk while they're read, so 12+ megapixel phone photos and large PNGs
  work (big PNGs take a few seconds). Phone photos are turned upright using their EXIF rotation.
- **Controls** (tap the picture): previous / next, pause, time per picture (5 s to 5 min),
  shuffle, Fit (whole picture) or Fill (fills the screen, edges cropped), Full screen, and
  **delete** (asks first, then erases the file from the card). The bar hides itself after 5 s.
- The screen timeout is skipped while Photos is open, so it works as a photo frame.

### Files

A file manager for the SD card. It opens in the `dashboard` folder, and **Up** goes as far as the
top of the card.

| | |
|---|---|
| ![Files](docs/screenshots/files.png) | ![File menu](docs/screenshots/files_menu.png) |
| ![Viewing a picture](docs/screenshots/files_picture.png) | |

- Shows each item's size and date, the number of items, and the card's free space. Sort by name,
  date (newest first) or size (biggest first); folders come first. Large folders are shown 50 at a
  time.
- Tap to open: folders, pictures (full screen, previous / next, delete) and text files (the first
  32 KB). Any file can be viewed as text or as hex (first 4 KB).
- The row menu (the list button, or long-press a row) shows details and offers **Rename**,
  **Delete** (folders with everything in them, after asking) and the viewers. **+ Folder** makes a
  new folder.
- Safety: the dashboard's own folders can't be renamed or deleted, and files named `wifi*.json`
  (which may hold Wi-Fi passwords) are never shown.

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
| ![About](docs/screenshots/settings_about.png) | ![Screensaver setting](docs/screenshots/settings_screensaver.png) |

Tabs for Network (Wi-Fi status, signal and your saved networks, with Remove), Display (screen timeout, info ticker, FPS meter),
Time (12/24-hour, time source), Weather (default city, ticker cities), St0nks (show the watchlist
in the ticker) and About (chip, memory, versions, settings backup). Settings are stored in the ESP32's NVS flash
area and survive reboots and reflashing.

## Building and flashing

You need [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/index.html).
LVGL 8.4 and the touch drivers are included in `components/`.

1. **Enable exFAT in ESP-IDF** (one-time; see [exFAT support](#exfat-support) below).
2. *Optional:* create `main/wifi_secrets.h` with a built-in Wi-Fi network (2.4 GHz only). This
   file is in `.gitignore` so it never gets committed. You can skip it and add networks from the
   SD card instead (see [Wi-Fi networks](#wi-fi-networks)).
   ```c
   #pragma once
   #define WIFI_SSID     "your network name"
   #define WIFI_PASSWORD "your password"
   ```
3. Build and flash (board connected over USB-C):
   ```bash
   . ~/esp/esp-idf/export.sh
   idf.py build
   idf.py -p /dev/ttyACM0 flash
   ```
4. To watch the log output: `idf.py -p /dev/ttyACM0 monitor` (Ctrl+] to exit). The log shows
   your Wi-Fi network's name and IP address, so don't paste it anywhere public.

If flashing can't connect, hold **BOOT**, tap **RESET**, release BOOT and try again, then press
RESET once afterwards to start the app.

### exFAT support

Most microSD cards over 32 GB come formatted as **exFAT**. ESP-IDF supports exFAT, but ships with it
switched off and has no menuconfig setting for it, so you need to change one line in your ESP-IDF
install. Edit `$IDF_PATH/components/fatfs/src/ffconf.h` (for example
`~/esp/esp-idf/components/fatfs/src/ffconf.h`) and change

```c
#define FF_FS_EXFAT		0
```

to

```c
#define FF_FS_EXFAT		1
```

or run:

```bash
sed -i 's/^#define FF_FS_EXFAT\t\t0$/#define FF_FS_EXFAT\t\t1/' ~/esp/esp-idf/components/fatfs/src/ffconf.h
```

The build stops with an error that points here if this hasn't been done. Redo it after updating or
reinstalling ESP-IDF. (The project's `sdkconfig` also enables `CONFIG_FATFS_USE_LABEL`, which ESP-IDF
5.5 needs to compile with exFAT on.)

The flash is split into the settings area (NVS), a 6 MB app partition and a ~9.9 MB `storage`
partition reserved for future use (see `partitions.csv`).

## SD card

Insert a microSD card (FAT32 or exFAT) and restart the display (it only checks for a card at
startup). It creates one folder, `dashboard`, and never touches anything else on the card:

| Folder | What it's for |
|---|---|
| `dashboard/import/` | One-time inbox. Files here are read at startup, applied, then **erased**. |
| `dashboard/pictures/` | Pictures for the [Photos](#photos) slideshow and the screensaver (JPEG, PNG, GIF, BMP; folders inside are included). |
| `dashboard/screenshots/` | Screenshots: long-press the top bar on any screen (see [Taking screenshots](#taking-screenshots)). |
| `dashboard/backup/` | Settings backups (see [Backing up settings](#backing-up-settings)). Never include Wi-Fi networks. |
| `dashboard/apps/` | Reserved for apps loaded from the card. |

A `README.txt` in the `dashboard` folder explains the same thing.

### Wi-Fi networks

The display can remember up to **10 Wi-Fi networks** and connects to the **strongest one in range**,
so it can move between home, work and a phone hotspot. (2.4 GHz networks only; hidden networks
aren't supported.) A network set in `main/wifi_secrets.h` is also used, as a built-in extra.

**To add a network on the display:** **Settings > Network > Add network**. The display scans and
lists the networks in range (strongest first). Tap one, type its password with the on-screen
keyboard (tick **Show** to check what you typed) and tap **Save**. Open networks just need a
confirmation. Enterprise (work login) and WEP networks aren't supported.

**To add several at once from the SD card:**

1. With the card in your PC, create a plain text file named **`wifi.json`** in the
   `dashboard/import/` folder (insert the card in the display once first, so the folder exists).
   Use a plain text editor (Notepad, gedit, VS Code), not a word processor:

   ```json
   {
     "networks": [
       { "ssid": "Home-Network",  "password": "home password" },
       { "ssid": "Work-Guest",    "password": "work password" },
       { "ssid": "Coffee-Shop",   "password": "" }
     ]
   }
   ```

   - `ssid` is the network name, exactly as it appears on your phone (capitals matter).
   - `password` is 8-63 characters, or `""` for an open network.
   - Watch the punctuation: quotes around every name and password, a comma between networks,
     and no comma after the last one.
   - If Windows saves it as `wifi.json.txt`, that's fine too.
2. Put the card in the display and restart it (press **RESET** or unplug it for a moment).
3. A message at the bottom of the screen says **"Imported N Wi-Fi networks from the SD card"**.
   The networks now appear in **Settings > Network**.

**What happens to the file:** once the networks are saved inside the display, `wifi.json` is
**overwritten with blanks and then deleted**, so the passwords don't stay on the card (a plain
delete would leave them recoverable with undelete tools). The display doesn't need the file
after that, and the card can be removed.

**If something's wrong** (a typo in the file, a password that's too short, or more than 10
networks in total), nothing is imported, the file is left on the card, and a message on screen
says what to fix.

**To change a password:** import a `wifi.json` with the same network name and the new password.

**To remove a network:** **Settings > Network**, tap **Remove** next to it and confirm. It's
deleted from the display's memory straight away, and if it was the network in use, the display
switches to another saved network.

**To erase them for good:** **Settings > Network > Securely wipe all saved networks**. Remove takes
a network off the list, but like most flash storage, the display's settings area only *marks* old
data as deleted; the password stays in the chip until that space is reused. The wipe copies your
other settings aside, erases the whole settings area, and puts the other settings back, so every
saved and previously removed network is really gone (the built-in network isn't affected).

Saved networks live in the ESP32's internal NVS storage, like the other settings. That storage
isn't encrypted, so treat the device itself like anything else that knows your Wi-Fi password.

### Backing up settings

**Settings > About > Back up settings to the SD card** saves every setting to
`dashboard/backup/settings_<date>_<time>.json`: clock format, screen timeout, ticker and FPS
options, weather cities, the St0nks watchlist, game levels and best scores. **Wi-Fi networks and
passwords are never included.**

**To restore:** copy a backup file from `dashboard/backup/` into `dashboard/import/` (keep the
`settings....json` name) and restart the display. It replaces the current settings, deletes the
copy in `import/` (the original in `backup/` stays), shows "Restored N settings" and restarts once
more so every app picks up the restored values. If the file is damaged or isn't a backup, nothing
is changed, the file stays, and a message says why.

The backup is plain text, so you can look at it (or carefully edit it) on your PC.

## Taking screenshots

Long-press an empty part of the top bar on any screen (the display pauses for a few seconds).

- **With an SD card in:** the screen is saved as a BMP image in `dashboard/screenshots/`, named
  after the date and time (for example `shot_20261019_213005.bmp`), and a message shows the name.
  Any image viewer opens BMP files.
- **Without a card:** the screen is sent over the USB cable instead. On the PC, run the receiver,
  which saves each one as a PNG in `docs/screenshots/`:

```bash
~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/screenshot.py
```

(Use the Python that comes with ESP-IDF, which has `pyserial`, and close `idf.py monitor` first.)

To copy screenshots from the card without taking it out, see the next section.

## Copying files over USB

`tools/sdcard.py` copies files to and from the SD card while it stays in the display, over the
same USB cable used for flashing. Only the card's `dashboard/` folder can be reached.

```bash
PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
$PY tools/sdcard.py ls pictures                  # list a folder (paths are inside dashboard/)
$PY tools/sdcard.py put *.jpg pictures/          # PC -> card
$PY tools/sdcard.py get screenshots ./shots      # card -> PC (one file or a whole folder)
$PY tools/sdcard.py rm pictures/old.jpg          # a file or an empty folder
$PY tools/sdcard.py mkdir pictures/holiday
```

- About 200 KB/s each way. Every 2 KB chunk is checked and acknowledged, so a damaged or lost chunk
  is sent again, and a file is only saved once its whole-file checksum matches. Interrupted copies
  leave nothing behind.
- `put` won't replace an existing file unless you add `--force`. File names must be plain ASCII.
- The display shows a message for each file it receives or deletes.
- Files named `wifi*.json` are never sent to the PC, and the dashboard's own folders can't be
  removed. Close `idf.py monitor` and `screenshot.py` first: only one program can use the port.
- A `wifi.json` put into `import/` is imported (and securely erased) at the next restart, the same
  as one copied onto the card.

## Project layout

| Path | What's there |
|---|---|
| `main/launcher.c` | Home screen, folders, app screens, screen timeout |
| `main/app_*.c` | One file per app |
| `main/chess_engine.c`, `main/checkers_engine.c` | Game engines (plain C, no LVGL; tests in `test/host/` run on a PC) |
| `main/ticker.c` | Home screen info ticker |
| `main/settings.c` | Saved settings (NVS) |
| `main/net_time.c`, `main/rtc.c` | Wi-Fi, NTP time and the RTC chip |
| `main/sd_card.c` | microSD card (FAT32 / exFAT): `dashboard` folders and the import inbox |
| `main/wifi_list.c` | Saved Wi-Fi networks (up to 10, in NVS) |
| `main/settings_backup.c` | Settings backup / restore on the SD card |
| `main/photo_frame.c`, `main/pic_decode.c` | Slideshow engine (Photos app and screensaver); JPEG/BMP and PNG/GIF decoding |
| `main/app_files.c` | File manager |
| `main/waveshare_rgb_lcd_port.c`, `main/lvgl_port.c` | Display, touch and LVGL setup, from Waveshare's demo |
| `main/usb_files.c`, `tools/sdcard.py` | SD card file transfer over USB |
| `tools/` | Image generators, the screenshot receiver and the USB file tool |
| `CLAUDE.md` | Detailed developer notes |

## Credits

- Started from Waveshare's ESP-IDF LVGL porting demo for this board.
- [LVGL](https://lvgl.io/) 8.4 (MIT licence).
- Weather data: [Open-Meteo](https://open-meteo.com/) (CC BY 4.0).
- Stock prices: Yahoo Finance; earnings dates: Nasdaq. For personal use; data is delayed and not
  investment advice.
- St0nks icon built from [Twemoji](https://github.com/twitter/twemoji) graphics by Twitter (CC BY 4.0).
- Fonts: Montserrat (bundled with LVGL).
