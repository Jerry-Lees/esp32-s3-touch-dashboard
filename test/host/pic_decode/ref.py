import sys
from PIL import Image
f, step, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
import os
im = Image.open(f.replace(".png", ".ref.png") if os.path.exists(f.replace(".png", ".ref.png")) else f)
im.seek(0)
im = im.convert("RGBA")
bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
im = Image.alpha_composite(bg, im).convert("RGB")
# box average over step x step blocks (partial at edges), like the decoder
w, h = im.size
ow, oh = (w + step - 1) // step, (h + step - 1) // step
src = im.load()
dec = Image.open(out).load()
err = 0; worst = 0
for oy in range(oh):
    for ox in range(ow):
        s = [0, 0, 0]; n = 0
        for y in range(oy * step, min(h, oy * step + step)):
            for x in range(ox * step, min(w, ox * step + step)):
                p = src[x, y]; n += 1
                for c in range(3): s[c] += p[c]
        r = [v // n for v in s]
        q = dec[ox, oy]
        e = max(abs(r[0] - q[0]), abs(r[1] - q[1]), abs(r[2] - q[2]))
        worst = max(worst, e); err += e
print(f"{f.split('/')[-1]:16} step {step}: mean err {err / (ow * oh):.2f}, worst {worst}")
