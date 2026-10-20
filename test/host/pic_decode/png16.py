import zlib, struct, sys
from PIL import Image
d = sys.argv[1]
def chunk(t, data): return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
def write(name, w, h, ctype, depth, rows, interlace=0, extra=b""):
    raw = b"".join(b"\x00" + r for r in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)) + extra
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(f"{d}/{name}", "wb").write(png)
w, h = 97, 61
# 16-bit RGB with a colour key (tRNS) on one value; and 16-bit grey
rows16, rowsg, ref, refg = [], [], Image.new("RGB", (w, h)), Image.new("RGB", (w, h))
for y in range(h):
    r, g = b"", b""
    for x in range(w):
        R, G, B = x * 65535 // w, y * 65535 // h, 30000
        r += struct.pack(">HHH", R, G, B)
        V = (x * 7 + y * 3) * 65535 // (w * 7 + h * 3)
        g += struct.pack(">H", V)
        key = (x, y) == (10, 10)
        ref.putpixel((x, y), (0, 0, 0) if key else (R >> 8, G >> 8, B >> 8))
        refg.putpixel((x, y), (V >> 8,) * 3)
    rows16.append(r); rowsg.append(g)
write("rgb16.png", w, h, 2, 16, rows16, extra=chunk(b"tRNS", struct.pack(">HHH", 10 * 65535 // w, 10 * 65535 // h, 30000)))
ref.save(f"{d}/rgb16.ref.png")
write("grey16b.png", w, h, 0, 16, rowsg)
refg.save(f"{d}/grey16b.ref.png")
write("interlaced.png", w, h, 0, 16, rowsg, interlace=1)
