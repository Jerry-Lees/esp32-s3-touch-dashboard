#!/usr/bin/env python3
"""
App signing for the dashboard's SD card apps.

Run it with ESP-IDF's Python (it has the `cryptography` library):

  PY=~/.espressif/python_env/idf5.5_py3.14_env/bin/python

  $PY tools/appkey.py genkey                  # once: local-keys/app_signing_key.pem (private,
                                             # git-ignored) + keys/builtin_app_key.pub (public)
  $PY tools/appkey.py genkey --out ~/mykeys  # a key pair for someone else (the display names
                                             # a key after its .pub file name)
  $PY tools/appkey.py sign build.app.elf app.json -o clock.app   # signed app
  $PY tools/appkey.py sign build.app.elf app.json -o clock.app --unsigned
  $PY tools/appkey.py info clock.app          # header, signer fingerprint, signature check
  $PY tools/appkey.py fingerprint keys/builtin_app_key.pub

Usually tools/build_app.sh runs the sign step for you.

App file (.app) layout, little-endian:
  0   8  magic "DASHAPP1"
  8   2  header size (128)
  10  2  flags (bit 0: signed)
  12  4  ELF size
  16  32 name (ASCII, NUL padded)
  48  16 version
  64  8  tile symbol (UTF-8, e.g. an LVGL symbol), NUL padded
  72  4  tile colour 0xRRGGBB
  76  8  key id: first 8 bytes of SHA-256 of the signer's public key (DER)
  84  16 folder on the display ("Games"; empty = Custom Apps), NUL padded
  100 2  icon width  } 0 = no icon (the tile shows the symbol); at most 64 x 64
  102 2  icon height }
  104 4  icon size in bytes (width * height * 3)
  108 20 reserved, zero
  128    the ELF file
  then the icon, if any: LVGL TRUE_COLOR_ALPHA pixels for RGB565 (2 bytes of
         colour, little-endian, then 1 byte of alpha), row by row
  then, if signed: u16 signature length + ECDSA P-256 / SHA-256 signature (DER)
         over everything before it (header + ELF + icon).

app.json: name, version, symbol, color, and optionally "folder" and "icon" (a
PNG next to app.json, at most 64 x 64; transparent parts show the tile colour).
"""
import argparse
import getpass
import hashlib
import json
import os
import struct
import sys
import zlib

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRIV_DEFAULT = os.path.join(ROOT, "local-keys", "app_signing_key.pem")
PUB_DEFAULT = os.path.join(ROOT, "keys", "builtin_app_key.pub")
MAGIC = b"DASHAPP1"
HDR = 128
HDR_FMT = "<8sHHI32s16s8sI8s16sHHI20s"
ICON_MAX = 64

# Names for the tile symbol (LVGL's built-in symbols; see lv_symbol_def.h).
SYMBOLS = {
    "audio": 0xF001, "video": 0xF008, "list": 0xF00B, "ok": 0xF00C, "close": 0xF00D,
    "power": 0xF011, "settings": 0xF013, "home": 0xF015, "download": 0xF019,
    "refresh": 0xF021, "image": 0xF03E, "edit": 0xF304, "play": 0xF04B, "pause": 0xF04C,
    "stop": 0xF04D, "eye": 0xF06E, "warning": 0xF071, "shuffle": 0xF074, "up": 0xF077,
    "down": 0xF078, "loop": 0xF079, "directory": 0xF07B, "upload": 0xF093, "call": 0xF095,
    "cut": 0xF0C4, "copy": 0xF0C5, "save": 0xF0C7, "bars": 0xF0C9, "envelope": 0xF0E0,
    "charge": 0xF0E7, "paste": 0xF0EA, "bell": 0xF0F3, "keyboard": 0xF11C, "gps": 0xF124,
    "file": 0xF158, "wifi": 0xF1EB, "battery": 0xF240, "trash": 0xF2ED, "usb": 0xF287,
    "bluetooth": 0xF293, "sd": 0xF7C2,
}


class Fail(Exception):
    pass


def der_public(pub):
    return pub.public_bytes(serialization.Encoding.DER,
                            serialization.PublicFormat.SubjectPublicKeyInfo)


def key_id(pub):
    return hashlib.sha256(der_public(pub)).digest()[:8]


def fingerprint(pub):
    k = key_id(pub).hex().upper()
    return " ".join(k[i:i + 4] for i in range(0, 16, 4))


def load_private(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        raise Fail(f"can't read the signing key {path}: {e.strerror}\n"
                   "(make one with: appkey.py genkey)")
    try:
        return serialization.load_pem_private_key(data, None)
    except TypeError:                                   # it has a passphrase
        pw = getpass.getpass(f"Passphrase for {path}: ").encode()
        return serialization.load_pem_private_key(data, pw)


def load_public(path):
    pub = serialization.load_pem_public_key(open(path, "rb").read())
    if not isinstance(pub, ec.EllipticCurvePublicKey) or pub.curve.name != "secp256r1":
        raise Fail(f"{path}: not an ECDSA P-256 public key")
    return pub


def cmd_genkey(a):
    if a.out:
        priv_path = os.path.join(a.out, "app_signing_key.pem")
        pub_path = os.path.join(a.out, "app_signing_key.pub")
    else:
        priv_path, pub_path = PRIV_DEFAULT, PUB_DEFAULT
    if os.path.exists(priv_path) and not a.force:
        raise Fail(f"{priv_path} already exists (use --force to replace it; apps signed "
                   "with the old key would stop loading)")
    key = ec.generate_private_key(ec.SECP256R1())
    enc = serialization.NoEncryption()
    if a.passphrase:
        pw = getpass.getpass("New passphrase: ")
        if pw != getpass.getpass("Again: ") or not pw:
            raise Fail("the passphrases don't match")
        enc = serialization.BestAvailableEncryption(pw.encode())
    os.makedirs(os.path.dirname(priv_path), exist_ok=True)
    os.makedirs(os.path.dirname(pub_path), exist_ok=True)
    fd = os.open(priv_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM,
                                  serialization.PrivateFormat.PKCS8, enc))
    with open(pub_path, "wb") as f:
        f.write(key.public_key().public_bytes(serialization.Encoding.PEM,
                                              serialization.PublicFormat.SubjectPublicKeyInfo))
    print(f"Private key: {priv_path}  (keep it secret; never commit it)")
    print(f"Public key:  {pub_path}")
    print(f"Fingerprint: {fingerprint(key.public_key())}")


def read_png(path):
    """8-bit RGB or RGBA, non-interlaced PNG -> (w, h, list of (r, g, b, a)).
    Only the standard library, so ESP-IDF's Python can run it."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise Fail(f"{path} isn't a PNG file")
    pos, idat, ihdr = 8, b"", None
    while pos + 8 <= len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat += chunk
        pos += 12 + n
    if not ihdr:
        raise Fail(f"{path}: damaged PNG")
    w, h, depth, ctype, _, _, interlace = ihdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise Fail(f"{path}: save the icon as an 8-bit RGB or RGBA PNG, not interlaced")
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    px, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - bpp] if i >= bpp else 0
            up = prev[i]
            ul = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + left) & 255
            elif f == 2:
                line[i] = (line[i] + up) & 255
            elif f == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif f == 4:
                p = left + up - ul
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - ul)
                pr = left if pa <= pb and pa <= pc else up if pb <= pc else ul
                line[i] = (line[i] + pr) & 255
        for x in range(w):
            q = line[x * bpp:x * bpp + bpp]
            px.append((q[0], q[1], q[2], q[3] if bpp == 4 else 255))
        prev = line
    return w, h, px


def icon_pixels(path):
    """PNG -> (w, h, LVGL RGB565 + alpha bytes)."""
    w, h, px = read_png(path)
    if w > ICON_MAX or h > ICON_MAX:
        raise Fail(f"{path}: the icon is {w} x {h}; at most {ICON_MAX} x {ICON_MAX}")
    out = bytearray()
    for r, g, b, al in px:
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += struct.pack("<HB", c, al)
    return w, h, bytes(out)


def cmd_sign(a):
    elf = open(a.elf, "rb").read()
    if elf[:4] != b"\x7fELF":
        raise Fail(f"{a.elf} isn't an ELF file")
    meta = json.load(open(a.meta))
    name = meta["name"].encode("ascii")
    version = str(meta.get("version", "1.0")).encode("ascii")
    sym = meta.get("symbol", "play")
    if sym not in SYMBOLS:
        raise Fail(f"unknown symbol '{sym}'; choose one of: {', '.join(sorted(SYMBOLS))}")
    symbol = chr(SYMBOLS[sym]).encode("utf-8")
    color = int(str(meta.get("color", "0x546E7A")), 16)
    folder = meta.get("folder", "").encode("ascii")
    if not 0 < len(name) < 32 or len(version) >= 16 or len(folder) >= 16:
        raise Fail("name must be 1-31 characters, version and folder at most 15")
    iw = ih = 0
    icon = b""
    if meta.get("icon"):
        iw, ih, icon = icon_pixels(os.path.join(os.path.dirname(os.path.abspath(a.meta)),
                                                meta["icon"]))

    key = None if a.unsigned else load_private(a.key)
    kid = key_id(key.public_key()) if key else bytes(8)
    hdr = struct.pack(HDR_FMT, MAGIC, HDR, 0 if a.unsigned else 1, len(elf),
                      name, version, symbol, color, kid, folder, iw, ih, len(icon), bytes(20))
    assert len(hdr) == HDR
    body = hdr + elf + icon
    if key:
        sig = key.sign(body, ec.ECDSA(hashes.SHA256()))
        body += struct.pack("<H", len(sig)) + sig
    with open(a.output, "wb") as f:
        f.write(body)
    who = f"signed, key {fingerprint(key.public_key())}" if key else "UNSIGNED"
    print(f"{a.output}: {meta['name']} {version.decode()}, {len(elf):,} bytes of code, {who}")


def cmd_info(a):
    data = open(a.app, "rb").read()
    if len(data) < HDR or data[:8] != MAGIC:
        raise Fail("not a dashboard app file")
    (_, hs, flags, size, name, version, symbol, color, kid, folder, iw, ih, isize, _) = \
        struct.unpack(HDR_FMT, data[:HDR])
    print(f"name {name.rstrip(b'\0').decode()}  version {version.rstrip(b'\0').decode()}  "
          f"colour #{color:06X}  code {size:,} bytes")
    if folder.rstrip(b"\0"):
        print(f"folder {folder.rstrip(b'\0').decode()}")
    if isize:
        print(f"icon {iw} x {ih}")
    if not flags & 1:
        print("UNSIGNED (only runs in developer mode, after approval on the display)")
        return
    k = kid.hex().upper()
    print("signer key id", " ".join(k[i:i + 4] for i in range(0, 16, 4)))
    end = HDR + size + isize
    (slen,) = struct.unpack("<H", data[end:end + 2])
    sig = data[end + 2:end + 2 + slen]
    pub = load_public(a.key)
    if key_id(pub) != kid:
        print(f"(not signed by {a.key})")
        return
    try:
        pub.verify(sig, data[:end], ec.ECDSA(hashes.SHA256()))
        print(f"signature OK ({a.key})")
    except InvalidSignature:
        print("SIGNATURE DOES NOT MATCH: the file was changed after signing")
        sys.exit(1)


def cmd_fingerprint(a):
    print(fingerprint(load_public(a.pub)))


def main():
    ap = argparse.ArgumentParser(description="Sign apps for the dashboard's SD card.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("genkey", help="make a signing key pair")
    g.add_argument("--out", help="folder for someone else's key pair (default: this "
                                 "project's built-in key)")
    g.add_argument("--passphrase", action="store_true", help="protect the private key")
    g.add_argument("--force", action="store_true")
    s = sub.add_parser("sign", help="wrap (and sign) an app")
    s.add_argument("elf")
    s.add_argument("meta", help="app.json: name, version, symbol, color, folder, icon")
    s.add_argument("-o", "--output", required=True)
    s.add_argument("--key", default=PRIV_DEFAULT)
    s.add_argument("--unsigned", action="store_true", help="for developer mode")
    i = sub.add_parser("info", help="show an app file and check its signature")
    i.add_argument("app")
    i.add_argument("--key", default=PUB_DEFAULT, help="public key to check against")
    f = sub.add_parser("fingerprint", help="show a public key's fingerprint")
    f.add_argument("pub")
    a = ap.parse_args()
    try:
        {"genkey": cmd_genkey, "sign": cmd_sign, "info": cmd_info,
         "fingerprint": cmd_fingerprint}[a.cmd](a)
    except (Fail, KeyError, ValueError, OSError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
