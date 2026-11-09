#!/usr/bin/env python3
"""Builds local-assets/cups.js from the game's own cup meshes and textures, for a local copy of the preview.

The output holds Bethesda's assets, so it stays on the player's machine (local-assets/ is git-ignored) and is never
published. Give it a folder holding the .nif and .dds files (any file-name prefix is fine):

    python3 tools/make_local_cups.py <folder>
    LOCAL=1 ./preview/build.sh      # -> preview/yacht-local.html
"""
import base64
import glob
import io
import json
import os
import subprocess
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# Measured from the meshes (tools/nif2json.py + the up-facing triangles): the inside floor and rim heights, and the
# inside radius at the rim, in game units. Each cup is scaled so its inside radius matches the physics cup's.
CUPS = {
    "tankard": {"nif": "basictankard01", "floor": 2.9, "top": 12.1, "inner": 4.32, "room": 1.22,
                "map": "diningset01", "normal": "diningset01_n", "metalness": 0.3, "roughness": 0.5},
    "dwemer": {"nif": "dwecup01", "floor": 0.8, "top": 10.4, "inner": 4.36, "room": 1.22,
               "map": "dwemetalsheet01", "normal": "dwemetalsheet01_n", "metalness": 0.45, "roughness": 0.5},
    "goblet": {"nif": "glazedgoblet01", "floor": 6.6, "top": 15.0, "inner": 4.6, "room": 1.1,
               "map": "glazedpottery01", "normal": "glazedpottery01_n", "metalness": 0.0, "roughness": 0.4},
    "silver": {"nif": "silvergoblet01", "floor": 7.8, "top": 19.3, "inner": 5.2, "room": 1.1,
               "map": "katariahclutter01", "normal": "sovngardeclutter01_n", "metalness": 0.6, "roughness": 0.32,
               "tint": "#e6ecff"},
    # Thieves Guild golden goblet: a glassy layer plus two gold parts. Per-shape materials; textures are optional.
    "golden": {"nif": "tggoldengoblet01", "floor": 8.3, "top": 20.34, "inner": 4.19, "room": 1.1, "metalness": 1.0, "roughness": 0.25,
               "shape_mats": [
                   {"color": "#c9a24a", "metalness": 0.2, "roughness": 0.08, "opacity": 0.35},
                   {"color": "#e8b84a", "metalness": 1.0, "roughness": 0.25, "map": "gold01", "normal": "gold01_n"},
                   {"color": "#f0c860", "metalness": 1.0, "roughness": 0.2, "map": "goldtrim01", "normal": "goldtrim01_n"},
               ]},
}
CUP_RADIUS = 1.9  # DiceSpec::cupRadius


def find(folder, stem, ext):
    for p in glob.glob(os.path.join(folder, "*")):
        base = os.path.basename(p).lower()
        if base.endswith(stem + ext) and (base == stem + ext or base[-len(stem + ext) - 1] in "-_ "):
            return p
    return None


TEX_SIZE = 2048


def png_uri(path, size=TEX_SIZE, normal=False):
    """The texture at size x size (Lanczos; colour maps get a light sharpen), as a high-quality JPEG data URI."""
    from PIL import ImageFilter
    im = Image.open(path)
    im.load()
    im = im.convert("RGB")
    if im.size != (size, size):
        im = im.resize((size, size), Image.LANCZOS)
        if not normal:
            im = im.filter(ImageFilter.UnsharpMask(radius=1.6, percent=55, threshold=2))
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=93, subsampling=0 if normal else 2, optimize=True)
    return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()


def main(folder):
    out_dir = os.path.join(ROOT, "local-assets")
    os.makedirs(out_dir, exist_ok=True)
    cups = {}
    for cid, c in CUPS.items():
        nif = find(folder, c["nif"], ".nif")
        if not nif:
            print(f"{cid}: {c['nif']}.nif not found, skipped")
            continue
        js = os.path.join(out_dir, c["nif"] + ".json")
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "nif2json.py"), nif, js], check=True)
        mesh = json.load(open(js))
        # A little extra room: the meshes taper below the rim, and a die's corners must stay inside the wall.
        scale = CUP_RADIUS / c["inner"] * c.get("room", 1.0)
        entry = {"shapes": mesh["shapes"], "scale": scale, "z": -c["floor"] * scale, "height": (c["top"] - c["floor"]) * scale,
                 "metalness": c["metalness"], "roughness": c["roughness"], "tint": c.get("tint", "#ffffff")}
        for key in ("map", "normal", "metal"):
            if key in c:
                tex = find(folder, c[key], ".dds")
                if tex:
                    entry[key] = png_uri(tex, normal=key == "normal")
                else:
                    print(f"{cid}: {c[key]}.dds not found, left out")
        if "shape_mats" in c:
            mats = []
            for m in c["shape_mats"]:
                mm = {k: v for k, v in m.items() if k not in ("map", "normal")}
                for key in ("map", "normal"):
                    if key in m:
                        tex = find(folder, m[key], ".dds")
                        if tex:
                            mm[key] = png_uri(tex, normal=key == "normal")
                mats.append(mm)
            entry["mats"] = mats
        for s in entry["shapes"]:
            s.pop("textures", None)
        cups[cid] = entry
        print(f"{cid}: scale {scale:.3f}, inside depth {entry['height']:.2f}")
    with open(os.path.join(out_dir, "cups.js"), "w") as f:
        f.write("window.LOCAL_CUPS = " + json.dumps(cups) + ";\n")
    print("wrote local-assets/cups.js")


if __name__ == "__main__":
    main(sys.argv[1])
