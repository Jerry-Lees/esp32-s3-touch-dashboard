#!/usr/bin/env python3
"""
Saves screenshots sent by the display (long-press the top bar on any screen;
see main/screenshot.c) as PNG files. Leave it running, take as many
screenshots as you like on the device, then press Ctrl+C.

Run from the project root with ESP-IDF's Python (it has pyserial):
    ~/.espressif/python_env/idf5.5_py3.14_env/bin/python tools/screenshot.py [port] [out_dir]

Defaults: /dev/ttyACM0 and docs/screenshots. Files are shot_01.png, shot_02.png
and so on (numbering continues after the files already there). Don't run
idf.py monitor at the same time: only one program can use the port.
"""
import base64
import os
import re
import struct
import sys
import zlib

import serial


def write_png(path, w, h, rgb565):
    """RGB565 little-endian pixels -> 8-bit RGB PNG (no extra libraries)."""
    rows = bytearray()
    px = struct.unpack("<%dH" % (w * h), rgb565)
    for y in range(h):
        rows.append(0)                          # filter: none
        for v in px[y * w:(y + 1) * w]:
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            rows += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))

    def chunk(kind, data):
        c = kind + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 9)))
        f.write(chunk(b"IEND", b""))


def next_index(out_dir):
    used = [int(m.group(1)) for f in os.listdir(out_dir)
            if (m := re.fullmatch(r"shot_(\d+)\.png", f))]
    return max(used, default=0) + 1


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "docs/screenshots"
    os.makedirs(out_dir, exist_ok=True)

    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 1
    ser.dtr = True      # this DTR/RTS state doesn't reset the board
    ser.rts = False
    ser.open()
    print(f"Listening on {port}. Long-press the top bar on the display to take a "
          f"screenshot. Ctrl+C to stop.", flush=True)

    shot = None         # (w, h, chunks, {n: bytes}) while receiving
    try:
        while True:
            line = ser.readline().decode("ascii", "replace").strip()
            if not line:
                continue
            if line.startswith("SHOT-BEGIN"):
                _, w, h, n = line.split()
                shot = (int(w), int(h), int(n), {})
                print("Receiving...", end="", flush=True)
            elif line == "SHOT-END" and shot:
                w, h, n, parts = shot
                shot = None
                missing = [i for i in range(n) if i not in parts]
                if missing:
                    print(f" incomplete ({len(missing)} of {n} lines lost), try again", flush=True)
                    continue
                data = b"".join(parts[i] for i in range(n))[:w * h * 2]
                path = os.path.join(out_dir, "shot_%02d.png" % next_index(out_dir))
                write_png(path, w, h, data)
                print(" saved", path, flush=True)
            elif shot and (m := re.fullmatch(r"SHOT-(\d+):([A-Za-z0-9+/=]+)", line)):
                try:
                    shot[3][int(m.group(1))] = base64.b64decode(m.group(2))
                except ValueError:
                    pass
    except KeyboardInterrupt:
        print()
    finally:
        ser.close()


if __name__ == "__main__":
    main()
