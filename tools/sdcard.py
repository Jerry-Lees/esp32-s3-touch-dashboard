#!/usr/bin/env python3
"""
Copy files to and from the display's SD card over the USB cable, without
taking the card out. Only the card's dashboard/ folder can be reached.

Run it with ESP-IDF's Python (it has pyserial), with nothing else using the
port (close any `idf.py monitor` first):

  PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python
  $PY tools/sdcard.py ls                         # dashboard/
  $PY tools/sdcard.py ls pictures
  $PY tools/sdcard.py put a.jpg b.png pictures/  # PC -> card (into a folder)
  $PY tools/sdcard.py put a.jpg pictures/x.jpg   # ... or to a new name
  $PY tools/sdcard.py get screenshots ./shots    # card -> PC (a file or a whole folder)
  $PY tools/sdcard.py rm pictures/x.jpg          # a file or an empty folder
  $PY tools/sdcard.py mkdir pictures/holiday

Options: -p PORT (default /dev/ttyACM0), --force (put: replace existing files).
Files named wifi*.json are never sent from the display.
"""
import argparse
import base64
import os
import sys
import time
import zlib
from urllib.parse import quote, unquote_to_bytes

import serial

CHUNK = 2048       # must not exceed CHUNK in main/usb_files.c
TIMEOUT = 3.0      # seconds to wait for an answer before resending
RETRIES = 5


class Fail(Exception):
    pass


def enc(path):
    """Path for the display: percent-encoded, names in the card's code page."""
    return quote(path.strip("/").encode("cp437"), safe="/") if path.strip("/") else ""


def dec(name):
    return unquote_to_bytes(name).decode("cp437")


class Display:
    def __init__(self, port):
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.2
        self.ser.dtr = True     # this DTR/RTS state doesn't reset the board
        self.ser.rts = False
        try:
            self.ser.open()
        except serial.SerialException as e:
            raise Fail(f"can't open {port}: {e}\n(Is `idf.py monitor` or screenshot.py running?)")
        self.buf = b""

    def send(self, line):
        self.ser.write((line + "\n").encode("ascii"))

    def line(self, timeout=TIMEOUT):
        """Next protocol line ("@@..."), or None on timeout. Log lines are skipped."""
        end = time.monotonic() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                raw, self.buf = self.buf[:nl], self.buf[nl + 1:]
                text = raw.decode("ascii", "replace").strip()
                if text.startswith("@@"):
                    return text
                continue
            if time.monotonic() > end:
                return None
            self.buf += self.ser.read(self.ser.in_waiting or 1)

    def request(self, cmd, timeout=TIMEOUT):
        self.ser.reset_input_buffer()
        self.buf = b""
        self.send("@@sd " + cmd)
        while True:
            r = self.line(timeout)
            if r is None:
                raise Fail("the display didn't answer (is the latest firmware flashed?)")
            if r.startswith("@@err "):
                raise Fail(r[6:])
            if r.startswith("@@ok") or r.startswith("@@e "):
                return r

    def hello(self):
        try:
            r = self.request("hello")
        except Fail:
            time.sleep(1)               # just reset or replugged: one more try
            r = self.request("hello")
        if not r.endswith(" 1"):
            raise Fail("there's no SD card in the display (insert one and restart it)")

    def ls(self, path):
        first = self.request("ls " + enc(path))
        items = []
        r = first
        while not r.startswith("@@ok ls"):
            _, kind, size, date, name = r.split(" ", 4)
            items.append((kind == "D", int(size), date, dec(name)))
            r = self.line()
            if r is None:
                raise Fail("the listing stopped halfway")
            if r.startswith("@@err "):
                raise Fail(r[6:])
        return items

    def is_dir(self, path):
        if not path.strip("/"):
            return True
        parent, _, name = path.strip("/").rpartition("/")
        try:
            return any(d and n.lower() == name.lower() for d, _, _, n in self.ls(parent))
        except Fail:
            return False

    def get(self, remote, local):
        r = self.request("get " + enc(remote))
        size = int(r.split()[2])
        tmp = local + ".part"
        got, seq, total = 0, 0, 0
        with open(tmp, "wb") as f:
            while True:
                r = self.line(10)
                if r is None:
                    self.send("@@sd abort")
                    raise Fail("the display stopped sending")
                if r.startswith("@@err "):
                    raise Fail(r[6:])
                if r.startswith("@@end "):
                    break
                if not r.startswith("@@d "):
                    continue
                try:
                    _, s, c, b = r.split(" ", 3)
                    s, c, data = int(s), int(c, 16), base64.b64decode(b, validate=True)
                    good = zlib.crc32(data) == c
                except ValueError:
                    continue                       # garbled line: it's sent again
                if good and s == seq:
                    f.write(data)
                    total = zlib.crc32(data, total)
                    got += len(data)
                    seq += 1
                    self.send(f"@@sd ack {s}")
                    progress(remote, got, size)
                elif s + 1 == seq:
                    self.send(f"@@sd ack {s}")     # our ack got lost
                else:
                    self.send(f"@@sd nak {s}")
        if int(r.split()[1], 16) != total or got != size:
            os.remove(tmp)
            raise Fail("the file arrived damaged; try again")
        os.replace(tmp, local)
        done(remote, size)

    def put(self, local, remote, force):
        data = open(local, "rb").read()
        self.request(f"put {len(data)} {zlib.crc32(data):08x} {int(force)} {enc(remote)}")
        for seq, ofs in enumerate(range(0, len(data), CHUNK)):
            part = data[ofs:ofs + CHUNK]
            line = f"@@sd d {seq} {zlib.crc32(part):08x} {base64.b64encode(part).decode()}"
            for _ in range(RETRIES):
                self.send(line)
                r = self.line()
                while r is not None and r not in (f"@@ack {seq}", f"@@nak {seq}") \
                        and not r.startswith("@@err "):
                    r = self.line()
                if r is not None and r.startswith("@@err "):
                    raise Fail(r[6:])
                if r == f"@@ack {seq}":
                    break
            else:
                self.send("@@sd abort")
                raise Fail("the display stopped answering")
            progress(remote, ofs + len(part), len(data))
        while True:
            r = self.line(15)
            if r is None:
                raise Fail("no confirmation from the display")
            if r.startswith("@@err "):
                raise Fail(r[6:])
            if r == "@@ok done":
                break
        done(remote, len(data))


def progress(name, n, total):
    if sys.stdout.isatty():
        pct = 100 * n // total if total else 100
        print(f"\r  {name}: {pct}% ({n // 1024} of {total // 1024} KB)", end="", flush=True)


def done(name, size):
    if sys.stdout.isatty():
        print("\r\033[K", end="")
    print(f"  {name} ({size:,} bytes)")


def main():
    ap = argparse.ArgumentParser(description="Files on the display's SD card (dashboard/ folder) over USB.")
    ap.add_argument("-p", "--port", default="/dev/ttyACM0")
    ap.add_argument("--force", action="store_true", help="put: replace files that are already there")
    ap.add_argument("command", choices=["ls", "get", "put", "rm", "mkdir"])
    ap.add_argument("paths", nargs="*")
    a = ap.parse_args()

    try:
        d = Display(a.port)
        d.hello()
        if a.command == "ls":
            path = a.paths[0] if a.paths else ""
            items = sorted(d.ls(path), key=lambda i: (not i[0], i[3].lower()))
            print(f"dashboard/{path.strip('/')}" + ("/" if path.strip("/") else ""))
            for is_dir, size, date, name in items:
                print(f"  {date.replace('T', ' ')}  {'<folder>' if is_dir else f'{size:>12,}'}  "
                      f"{name}{'/' if is_dir else ''}")
            print(f"  {len(items)} item(s)")
        elif a.command == "get":
            if not a.paths or len(a.paths) > 2:
                raise Fail("usage: get REMOTE [LOCAL]")
            remote, local = a.paths[0].strip("/"), (a.paths[1] if len(a.paths) > 1 else ".")
            if d.is_dir(remote):
                os.makedirs(local, exist_ok=True)
                files = [n for is_dir, _, _, n in d.ls(remote) if not is_dir]
                for n in files:
                    try:
                        d.get(f"{remote}/{n}", os.path.join(local, n))
                    except Fail as e:
                        print(f"  {n}: skipped ({e})")
                print(f"{len(files)} file(s) from dashboard/{remote}")
            else:
                if os.path.isdir(local):
                    local = os.path.join(local, remote.rpartition("/")[2])
                d.get(remote, local)
        elif a.command == "put":
            if len(a.paths) < 2:
                raise Fail("usage: put LOCAL... REMOTE")
            *sources, dest = a.paths
            into_dir = dest.endswith("/") or len(sources) > 1 or d.is_dir(dest)
            for s in sources:
                if not os.path.isfile(s):
                    raise Fail(f"{s}: not a file")
                target = f"{dest.rstrip('/')}/{os.path.basename(s)}" if into_dir else dest
                d.put(s, target.lstrip("/"), a.force)
        elif a.command == "rm":
            for p in a.paths:
                d.request("rm " + enc(p))
                print(f"  deleted {p}")
        elif a.command == "mkdir":
            for p in a.paths:
                d.request("mkdir " + enc(p))
                print(f"  made {p}/")
    except Fail as e:
        print(f"\nError: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        try:
            d.send("@@sd abort")
        except Exception:
            pass
        sys.exit(130)


if __name__ == "__main__":
    main()
