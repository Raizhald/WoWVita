# Generates the VPK's LiveArea images (sce_sys): icon0.png 128x128, bg.png 840x500, startup.png 280x158.
# The Vita wants them as 8-bit palette PNGs. Plain generated art: a night sky, hills, the "WV" letters.
#   python3 tools/gen_sce_sys.py sce_sys [font.ttf]
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
TTF = sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def scene(w, h, title, sub=None):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):   # sky: deep blue to dusk teal
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=(int(12 + 20 * t), int(18 + 50 * t), int(40 + 60 * t)))
    for i, (amp, base, col) in enumerate([(0.10, 0.70, (24, 52, 44)), (0.08, 0.80, (18, 40, 30)), (0.06, 0.90, (12, 28, 20))]):
        pts = [(0, h)]
        for x in range(0, w + 1, 2):
            y = h * (base - amp * (0.6 * math.sin(x / w * 6.3 + i * 2.1) + 0.4 * math.sin(x / w * 15 + i)))
            pts.append((x, y))
        pts.append((w, h))
        d.polygon(pts, fill=col)
    size = int(h * (0.34 if sub else 0.42))
    font = ImageFont.truetype(TTF, size)
    tw = d.textlength(title, font=font)
    ty = h * (0.18 if sub else 0.16)
    glow = Image.new("L", (w, h), 0)
    ImageDraw.Draw(glow).text(((w - tw) / 2, ty), title, font=font, fill=255)
    glow = glow.filter(ImageFilter.GaussianBlur(max(2, size // 12)))
    im.paste((255, 170, 40), (0, 0), glow.point(lambda v: v * 0.6))
    d.text(((w - tw) / 2, ty), title, font=font, fill=(250, 210, 90), stroke_width=max(1, size // 24), stroke_fill=(90, 50, 10))
    if sub:
        sf = ImageFont.truetype(TTF, int(h * 0.075))
        sw = d.textlength(sub, font=sf)
        d.text(((w - sw) / 2, ty + size * 1.15), sub, font=sf, fill=(220, 225, 230))
    return im.quantize(colors=256, method=Image.Quantize.MEDIANCUT)


os.makedirs(os.path.join(OUT, "livearea", "contents"), exist_ok=True)
scene(128, 128, "WV").save(os.path.join(OUT, "icon0.png"), optimize=True)
scene(840, 500, "WV", "WoWVita - a 1.12.1 client for PS Vita").save(os.path.join(OUT, "livearea", "contents", "bg.png"), optimize=True)
scene(280, 158, "WV", "WoWVita").save(os.path.join(OUT, "livearea", "contents", "startup.png"), optimize=True)
