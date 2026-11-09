#!/usr/bin/env python3
"""The interface art for the SKSE plugin: the scorecards' parchment (as the preview's paintParchment), written as raw
RGBA with a small header (u32 width, u32 height) so the DLL can load it without an image decoder.

Out: assets/ui/parchment_a.rgba, parchment_b.rgba (two sheets, the opponent's and yours) and PNG copies to look at.
"""
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy.ndimage import zoom

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "ui")


def fbm(rng, w, h, scale, octaves):
    out = np.zeros((h, w))
    amp, total = 1.0, 0.0
    for o in range(octaves):
        s = scale / (2 ** o)
        gw, gh = max(2, int(w / s) + 2), max(2, int(h / s) + 2)
        g = rng.random((gh, gw))
        up = zoom(g, (h / gh * 1.0001, w / gw * 1.0001), order=3)[:h, :w]
        out += up * amp
        total += amp
        amp *= 0.5
    return out / total


def sheet(seed, W=768, H=560):
    rng = np.random.default_rng(seed)
    # ragged outline
    pts = []
    # a slow wobble with a little fine tear in it, not per-point jitter
    wob = np.convolve(rng.random(1200) - 0.5, np.ones(5) / 5, mode="same") * 22 + (rng.random(1200) - 0.5) * 3
    k = iter(range(10 ** 6))
    edge = lambda: wob[next(k) % 1200]
    step = 6
    for x in range(0, W + 1, step):
        pts.append((x, 10 + edge()))
    for y in range(0, H + 1, step):
        pts.append((W - 10 + edge(), y))
    for x in range(W, -1, -step):
        pts.append((x, H - 10 + edge()))
    for y in range(H, -1, -step):
        pts.append((10 + edge(), y))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    # the paper
    blot = fbm(rng, W, H, 120, 4)
    fine = fbm(rng, W, H, 6, 2)
    fib = zoom(rng.random((H // 26 + 2, W // 3 + 2)), (H / (H // 26 + 2) * 1.0001, W / (W // 3 + 2) * 1.0001), order=1)[:H, :W]
    n = (blot - 0.5) * 48 + (fine - 0.5) * 16 + (fib - 0.5) * 10 + (rng.random((H, W)) - 0.5) * 6
    base = np.array([230, 212, 174], dtype=np.float64)
    rgb = np.stack([base[0] + n, base[1] + n * 0.9, base[2] + n * 0.75], axis=-1)
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    # old stains: a cup ring, a few drips
    st = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(st)
    cx, cy, r = W * (0.62 + rng.random() * 0.2), H * (0.55 + rng.random() * 0.3), 46 + rng.random() * 14
    d.arc([cx - r, cy - r, cx + r, cy + r], 17, 320, fill=(120, 72, 30, 30), width=5)
    for _ in range(4):
        x, y, a, b = rng.random() * W, rng.random() * H, 30 + rng.random() * 70, 20 + rng.random() * 50
        d.ellipse([x - a, y - b, x + a, y + b], fill=(130, 80, 35, 18))
    img = Image.alpha_composite(img, st.filter(ImageFilter.GaussianBlur(6)))
    # darkened, slightly burnt edges
    burn = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bd = ImageDraw.Draw(burn)
    bd.line(pts + [pts[0]], fill=(92, 52, 20, 105), width=28)
    img = Image.alpha_composite(img, burn.filter(ImageFilter.GaussianBlur(10)))
    burn2 = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(burn2).line(pts + [pts[0]], fill=(48, 24, 8, 150), width=8)
    img = Image.alpha_composite(img, burn2.filter(ImageFilter.GaussianBlur(3)))
    img.putalpha(mask.filter(ImageFilter.GaussianBlur(0.8)))
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    for name, seed in (("parchment_a", 311), ("parchment_b", 977)):
        img = sheet(seed)
        img.save(os.path.join(OUT, name + ".png"))
        with open(os.path.join(OUT, name + ".rgba"), "wb") as f:
            f.write(struct.pack("<II", img.width, img.height))
            f.write(img.tobytes())
    print("wrote", OUT)


if __name__ == "__main__":
    main()
