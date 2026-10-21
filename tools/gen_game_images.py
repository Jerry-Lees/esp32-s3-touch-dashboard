#!/usr/bin/env python3
"""
Generates the games' pictures (the games are SD card apps in apps/):
  - apps/chess/main/chess_images.c        chess pieces
  - apps/checkers/main/checkers_images.c  the king's crown
  - apps/flappy/main/flappy_images.c      the bird
as LVGL v8 LV_IMG_CF_TRUE_COLOR_ALPHA images for RGB565 without byte swap,
and each game's 64 x 64 tile icon, apps/<game>/icon.png.

The pieces come from the chess glyphs in Noto Sans Symbols 2 (SIL Open Font
License). Each piece is drawn as a solid silhouette with the glyph's lines
on top, so white pieces are ivory with dark lines and black pieces are dark
with light lines.

Run from the project root:
    python3 -I tools/gen_game_images.py [preview.png]
"""
import sys
from PIL import Image, ImageDraw, ImageFont, ImageChops

FONT = "/usr/share/fonts/google-noto/NotoSansSymbols2-Regular.ttf"
# LVGL's copy of the font the display uses (for the 2048 icon).
MONTSERRAT = "components/lvgl__lvgl/scripts/built_in_font/Montserrat-Medium.ttf"
ICON = 64
SIZE = 44          # image size in pixels (board squares are 50 px)
CROWN_SIZE = 30

# Outline glyphs U+2654..2659 (K Q R B N P); the filled ones are +6.
PIECES = [("king", 0), ("queen", 1), ("rook", 2), ("bishop", 3),
          ("knight", 4), ("pawn", 5)]

IVORY = (246, 238, 220)
INK = (24, 24, 24)
DARK = (40, 40, 44)
LIGHT_LINE = (200, 200, 200)
GOLD = (255, 202, 40)
GOLD_LINE = (120, 80, 0)


def glyph_mask(ch, size):
    """Alpha mask of a glyph, centred in a size x size box."""
    font = ImageFont.truetype(FONT, int(size * 0.98))
    big = Image.new("L", (size * 2, size * 2), 0)
    d = ImageDraw.Draw(big)
    d.text((size // 2, size // 2), ch, font=font, fill=255)
    bbox = big.getbbox()
    g = big.crop(bbox)
    # Scale to fit with a small margin, keeping proportions.
    scale = min((size - 2) / g.width, (size - 2) / g.height)
    g = g.resize((max(1, round(g.width * scale)), max(1, round(g.height * scale))),
                 Image.LANCZOS)
    out = Image.new("L", (size, size), 0)
    out.paste(g, ((size - g.width) // 2, size - 1 - g.height))  # sit on the bottom
    return out


def silhouette(outline_mask):
    """Everything not reachable from the border without crossing a line."""
    size = outline_mask.width
    # Pad by one pixel so the flood fill can go all the way around.
    padded = Image.new("L", (size + 2, size + 2), 0)
    padded.paste(outline_mask.point(lambda v: 255 if v > 80 else 0), (1, 1))
    ImageDraw.floodfill(padded, (0, 0), 128)
    inside = padded.point(lambda v: 0 if v == 128 else 255).crop((1, 1, size + 1, size + 1))
    return inside


def compose(fill_color, fill_mask, line_color, line_mask):
    img = Image.new("RGBA", fill_mask.size, fill_color + (0,))
    img.putalpha(fill_mask)
    top = Image.new("RGBA", line_mask.size, line_color + (0,))
    top.putalpha(line_mask)
    return Image.alpha_composite(img, top)


def make_piece(index, white, size=SIZE):
    outline = glyph_mask(chr(0x2654 + index), size)   # white (outlined) glyph
    solid = glyph_mask(chr(0x265A + index), size)     # black (filled) glyph
    sil = ImageChops.lighter(silhouette(outline), solid)
    if white:
        return compose(IVORY, sil, INK, outline)
    return compose(LIGHT_LINE, sil, DARK, solid)


def make_crown():
    outline = glyph_mask("♕", CROWN_SIZE)
    sil = silhouette(outline)
    return compose(GOLD, sil, GOLD_LINE, outline)


def make_bird(wing_up):
    """48 x 36 bird, drawn 4x larger and scaled down for smooth edges."""
    S = 4
    img = Image.new("RGBA", (48 * S, 36 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    line = (40, 32, 28, 255)
    w = 2 * S

    def box(x1, y1, x2, y2):
        return [x1 * S, y1 * S, x2 * S, y2 * S]

    d.ellipse(box(3, 4, 37, 33), fill=(247, 210, 62, 255))           # body
    d.ellipse(box(7, 19, 33, 33), fill=(252, 236, 170, 255))         # belly
    d.ellipse(box(3, 4, 37, 33), outline=line, width=w)
    d.ellipse(box(23, 4, 37, 19), fill=(255, 255, 255, 255), outline=line, width=w)  # eye
    d.ellipse(box(30, 8, 35, 14), fill=line)                          # pupil
    d.polygon([(29 * S, 18 * S), (46 * S, 19 * S), (45 * S, 23 * S), (29 * S, 23 * S)],
              fill=(245, 124, 42, 255), outline=line, width=S)        # upper beak
    d.polygon([(29 * S, 23 * S), (43 * S, 23 * S), (41 * S, 28 * S), (29 * S, 27 * S)],
              fill=(226, 92, 30, 255), outline=line, width=S)         # lower beak
    if wing_up:
        d.ellipse(box(1, 7, 21, 20), fill=(255, 246, 213, 255), outline=line, width=w)
    else:
        d.ellipse(box(1, 17, 21, 30), fill=(255, 246, 213, 255), outline=line, width=w)
    return img.resize((48, 36), Image.LANCZOS)


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
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join("0x%02x" % v for v in data[i:i + 24]) + ",")
    return (
        "static const uint8_t %s_map[] = {\n%s\n};\n"
        "const lv_img_dsc_t %s = {\n"
        "    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n"
        "    .header.always_zero = 0,\n"
        "    .header.w = %d,\n"
        "    .header.h = %d,\n"
        "    .data_size = sizeof(%s_map),\n"
        "    .data = %s_map,\n"
        "};\n\n" % (name, "\n".join(lines), name, w, h, name, name))


def hexrgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255, 255)


def smooth(draw_fn):
    """Draws at 4x and scales down for smooth edges (like LVGL's)."""
    S = 4
    big = Image.new("RGBA", (ICON * S, ICON * S), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(big), S)
    return big.resize((ICON, ICON), Image.LANCZOS)


def icon_chess():
    return make_piece(4, True, ICON)                   # white knight


def icon_checkers():
    d = ICON * 5 // 8

    def draw(g, S):
        for x, y, fill, edge in ((0, ICON - d, 0xEDE3CF, 0x8C8270),
                                 (ICON - d, 0, 0xC62828, 0x7F1010)):
            g.ellipse([x * S, y * S, (x + d) * S - 1, (y + d) * S - 1], fill=hexrgb(fill),
                      outline=hexrgb(edge), width=3 * S)
    img = smooth(draw)
    crown = make_crown()
    img.alpha_composite(crown, (ICON - d + (d - crown.width) // 2, (d - crown.height) // 2))
    return img


def icon_flappy():
    bird = make_bird(True)
    h = round(bird.height * ICON / bird.width)
    img = Image.new("RGBA", (ICON, ICON), (0, 0, 0, 0))
    img.alpha_composite(bird.resize((ICON, h), Image.LANCZOS), (0, (ICON - h) // 2))
    return img


def icon_snake():
    u = ICON // 4
    pts = [(0, 3), (1, 3), (2, 3), (2, 2), (2, 1), (3, 1)]

    def draw(g, S):
        for i, (x, y) in enumerate(pts):
            x0, y0 = x * u, y * u - u // 2
            g.rounded_rectangle([x0 * S, y0 * S, (x0 + u) * S - 1, (y0 + u) * S - 1], 4 * S,
                                fill=hexrgb(0xFFFFFF if i == 5 else 0xE8F5E9))
        x0, y0 = 3 * u + 1, 3 * u - u // 2 + 1
        g.ellipse([x0 * S, y0 * S, (x0 + u - 2) * S - 1, (y0 + u - 2) * S - 1],
                  fill=hexrgb(0xE7471D))
    return smooth(draw)


def icon_2048():
    def draw(g, S):
        g.rounded_rectangle([0, 0, ICON * S - 1, ICON * S - 1], 10 * S, fill=hexrgb(0xEDC22E))
        font = ImageFont.truetype(MONTSERRAT, 20 * S)
        g.text((ICON * S // 2, ICON * S // 2), "2048", font=font, fill=(255, 255, 255, 255),
               anchor="mm")
    return smooth(draw)


def write_c(path, images):
    out = ["/* Generated by tools/gen_game_images.py; do not edit by hand.",
           " * Glyphs: Noto Sans Symbols 2 (SIL Open Font License 1.1). */",
           '#include "lvgl.h"', "",
           "#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0",
           '#error "these images were generated for RGB565 without byte swap"',
           "#endif", ""]
    for name, img in images:
        out.append(to_c(name, img))
    with open(path, "w") as f:
        f.write("\n".join(out))
    print("wrote %s (%d images)" % (path, len(images)))


def main():
    chess = []
    for name, idx in PIECES:
        chess.append(("img_chess_w_" + name, make_piece(idx, True)))
        chess.append(("img_chess_b_" + name, make_piece(idx, False)))
    write_c("apps/chess/main/chess_images.c", chess)
    write_c("apps/checkers/main/checkers_images.c", [("img_game_crown", make_crown())])
    write_c("apps/flappy/main/flappy_images.c", [("img_flappy_bird_0", make_bird(True)),
                                                 ("img_flappy_bird_1", make_bird(False))])
    for game, fn in (("chess", icon_chess), ("checkers", icon_checkers), ("flappy", icon_flappy),
                     ("snake", icon_snake), ("2048", icon_2048)):
        fn().save("apps/%s/icon.png" % game)
        print("wrote apps/%s/icon.png" % game)
    images = chess + [("crown", make_crown()), ("bird", make_bird(True))]

    if len(sys.argv) > 1:
        # Preview: pieces on light and dark squares.
        prev = Image.new("RGB", (SIZE * len(images) + 20, SIZE * 2 + 20), (0, 0, 0))
        for i, (_, img) in enumerate(images):
            for row, sq in enumerate(((240, 217, 181), (181, 136, 99))):
                tile = Image.new("RGBA", (SIZE, SIZE), sq + (255,))
                im = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
                im.paste(img, ((SIZE - img.width) // 2, (SIZE - img.height) // 2))
                tile = Image.alpha_composite(tile, im)
                prev.paste(tile.convert("RGB"), (10 + i * SIZE, 10 + row * SIZE))
        prev = prev.resize((prev.width * 2, prev.height * 2), Image.NEAREST)
        prev.save(sys.argv[1])
        print("wrote", sys.argv[1])


if __name__ == "__main__":
    main()
