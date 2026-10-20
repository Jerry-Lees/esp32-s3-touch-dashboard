#!/usr/bin/env python3
"""
Generates main/elf_icon.c: the "Custom Apps" folder icon, an elf sitting on
a shelf (long pointed ears, floppy cap, leaf collar, in greens and browns;
deliberately not a red-and-white Christmas elf). LVGL v8
LV_IMG_CF_TRUE_COLOR_ALPHA for RGB565 without byte swap, drawn 8x larger
and scaled down for smooth edges.

Run from the project root:
    python3 -I tools/gen_elf_icon.py [preview.png]
"""
import sys
from PIL import Image, ImageDraw

N = 64          # icon size
S = 8           # drawing scale

INK = (42, 30, 20, 255)
SKIN = (242, 201, 160, 255)
SKIN_DARK = (214, 160, 120, 255)
CHEEK = (232, 140, 120, 255)
HAT = (31, 111, 92, 255)
HAT_BAND = (139, 195, 74, 255)
TUNIC = (62, 142, 78, 255)
COLLAR = (156, 204, 101, 255)
LEGS = (46, 94, 58, 255)
BELT = (93, 64, 40, 255)
BUCKLE = (233, 190, 80, 255)
BOOT = (121, 78, 44, 255)
HAIR = (140, 90, 50, 255)
WOOD = (141, 90, 43, 255)
WOOD_HI = (184, 128, 70, 255)
WHITE = (255, 255, 255, 255)


def P(*pts):
    return [(x * S, y * S) for x, y in pts]


def B(x1, y1, x2, y2):
    return [x1 * S, y1 * S, x2 * S, y2 * S]


def make_elf():
    img = Image.new("RGBA", (N * S, N * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = S                                                   # outline width: 1 px

    # Shelf, with a bracket underneath.
    d.polygon(P((30, 55), (34, 55), (33, 63), (31, 63)), fill=WOOD, outline=INK, width=w)
    d.rounded_rectangle(B(3, 50, 61, 56), radius=S, fill=WOOD, outline=INK, width=w)
    d.line(P((5, 51.5), (59, 51.5)), fill=WOOD_HI, width=S)

    # Legs dangling over the edge, with curled pointed boots.
    for x0, x1, toe in ((27, 25, -1), (37, 39, 1)):
        d.line(P((x0, 46), (x1, 57)), fill=INK, width=5 * S)
        d.line(P((x0, 46), (x1, 57)), fill=LEGS, width=3 * S)
        # boot: a rounded foot pointing outwards, toe curling up
        d.ellipse(B(min(x1, x1 + 7 * toe) - 1, 56.5, max(x1, x1 + 7 * toe) + 1, 61),
                  fill=BOOT, outline=INK, width=w)
        tx = x1 + 7 * toe
        d.polygon(P((tx - 1.5 * toe, 57), (tx + 1.5 * toe, 54.5), (tx + 0.5 * toe, 58.5)),
                  fill=BOOT, outline=INK, width=w)

    # Tunic (sitting: wide at the hips), belt and buckle.
    d.polygon(P((24, 30), (40, 30), (46, 49), (18, 49)), fill=TUNIC, outline=INK, width=w)
    d.rectangle(B(20, 41, 44, 44), fill=BELT, outline=INK, width=w)
    d.rectangle(B(30, 40.5, 34, 44.5), fill=BUCKLE, outline=INK, width=w)

    # Arms resting on the knees, hands at the ends.
    for sx, ex in ((25, 17), (39, 47)):
        d.line(P((sx, 32), (ex, 43)), fill=INK, width=5 * S)
        d.line(P((sx, 32), (ex, 43)), fill=TUNIC, width=3 * S)
        d.ellipse(B(ex - 2.5, 42, ex + 2.5, 47), fill=SKIN, outline=INK, width=w)

    # Leaf collar: a ring of pointed leaves.
    leaves = []
    for i in range(7):
        x = 22 + i * 3.4
        leaves += [(x, 29), (x + 1.7, 34)]
    leaves += [(45.8, 29)]
    d.polygon(P(*leaves, (42, 27), (22, 27)), fill=COLLAR, outline=INK, width=w)

    # Ears: long and pointed, sweeping up and out.
    for side in (-1, 1):
        cx = 32 + side * 8
        tip = (32 + side * 23, 9)
        d.polygon(P((cx, 15), tip, (cx + side * 1, 25)), fill=SKIN, outline=INK, width=w)
        d.polygon(P((cx + side * 1, 18), (32 + side * 19, 12), (cx + side * 1.5, 22)),
                  fill=SKIN_DARK)

    # Head.
    d.ellipse(B(22, 10, 42, 30), fill=SKIN, outline=INK, width=w)
    # Hair peeking out under the cap.
    d.chord(B(22, 9, 42, 21), 180, 360, fill=HAIR)

    # Cap: pointed, flopping over to the right, with a leaf-green band.
    d.polygon(P((21, 15), (43, 15), (41, 8), (36, 2), (47, 0), (52, 5), (44, 6), (35, 6.5),
                (25, 8)), fill=HAT, outline=INK, width=w)
    d.rounded_rectangle(B(20, 13, 44, 17), radius=S, fill=HAT_BAND, outline=INK, width=w)
    d.ellipse(B(49.5, 3, 54, 7.5), fill=HAT_BAND, outline=INK, width=w)   # a little acorn-like tip

    # Face: a sideways, slightly mischievous look.
    for ex in (27.5, 36.5):
        d.ellipse(B(ex - 2, 18.5, ex + 2, 23), fill=WHITE, outline=INK, width=w)
        d.ellipse(B(ex - 0.5, 19.5, ex + 1.8, 22.5), fill=INK)
    d.ellipse(B(23.5, 23, 27, 26), fill=CHEEK)
    d.ellipse(B(37, 23, 40.5, 26), fill=CHEEK)
    d.arc(B(28, 21.5, 37, 27.5), 20, 160, fill=INK, width=w)
    d.ellipse(B(31, 22.5, 33.5, 24.5), fill=SKIN_DARK)                   # nose

    return img.resize((N, N), Image.LANCZOS)


def to_c(name, img):
    w, h = img.size
    data = bytearray()
    raw = img.tobytes()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        if a == 0:
            r = g = b = 0
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        data += bytes((c & 0xFF, c >> 8, a))
    lines = ["    " + ",".join("0x%02x" % v for v in data[i:i + 24]) + ","
             for i in range(0, len(data), 24)]
    return (
        "static const uint8_t %s_map[] = {\n%s\n};\n"
        "const lv_img_dsc_t %s = {\n"
        "    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n"
        "    .header.always_zero = 0,\n"
        "    .header.w = %d,\n"
        "    .header.h = %d,\n"
        "    .data_size = sizeof(%s_map),\n"
        "    .data = %s_map,\n"
        "};\n" % (name, "\n".join(lines), name, w, h, name, name))


def main():
    elf = make_elf()
    with open("main/elf_icon.c", "w") as f:
        f.write("/* Generated by tools/gen_elf_icon.py; do not edit by hand. */\n"
                '#include "lvgl.h"\n\n'
                "#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0\n"
                '#error "elf_icon.c was generated for RGB565 without byte swap"\n'
                "#endif\n\n" + to_c("img_elf_icon", elf))
    print("wrote main/elf_icon.c")
    if len(sys.argv) > 1:
        # Preview on the folder's tile colour, at 1x and 4x.
        tile = Image.new("RGBA", (N + 16, N + 16), (57, 73, 171, 255))
        tile.alpha_composite(elf, (8, 8))
        big = tile.resize((tile.width * 4, tile.height * 4), Image.NEAREST)
        prev = Image.new("RGB", (big.width + tile.width + 30, big.height + 20), (16, 20, 24))
        prev.paste(tile.convert("RGB"), (10, 10))
        prev.paste(big.convert("RGB"), (tile.width + 20, 10))
        prev.save(sys.argv[1])
        print("wrote", sys.argv[1])


if __name__ == "__main__":
    main()
