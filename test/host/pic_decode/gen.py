import sys, random
from PIL import Image, ImageDraw
d = sys.argv[1]
random.seed(1)
def base(w, h):
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // w, (y * 255) // h, ((x ^ y) * 7) & 255)
    dr = ImageDraw.Draw(im)
    dr.ellipse((w // 4, h // 4, w * 3 // 4, h * 3 // 4), fill=(250, 250, 30))
    return im
b = base(333, 211)
b.save(f"{d}/rgb.png")
b.convert("RGBA").save(f"{d}/rgba.png")
a = b.convert("RGBA"); a.putalpha(Image.linear_gradient("L").resize(b.size)); a.save(f"{d}/rgba_alpha.png")
b.convert("L").save(f"{d}/grey.png")
la = b.convert("LA"); la.putalpha(Image.linear_gradient("L").resize(b.size)); la.save(f"{d}/greya.png")
b.convert("1").save(f"{d}/bw1.png")
b.convert("L").convert("P", palette=Image.ADAPTIVE, colors=4).save(f"{d}/pal2.png", bits=2)
b.convert("L").convert("P", palette=Image.ADAPTIVE, colors=16).save(f"{d}/pal4.png", bits=4)
p = b.quantize(64); p.save(f"{d}/pal8.png")
p.info["transparency"] = 3; p.save(f"{d}/pal8_trns.png", transparency=3)
b.convert("I;16").save(f"{d}/grey16.png")
b.save(f"{d}/rgb_fast.png", compress_level=0)
big = base(1600, 1200); big.save(f"{d}/big.png")
q = b.quantize(200); q.save(f"{d}/plain.gif", interlace=False)
q.save(f"{d}/interlaced.gif", interlace=True)
q.save(f"{d}/trans.gif", transparency=5)
small = b.convert("L").quantize(4); small.save(f"{d}/small_pal.gif")
f2 = base(333, 211).transpose(Image.FLIP_LEFT_RIGHT).quantize(200)
q.save(f"{d}/anim.gif", save_all=True, append_images=[f2], duration=200, loop=0)
