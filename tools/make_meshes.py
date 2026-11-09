#!/usr/bin/env python3
"""Builds the game's meshes and textures for the tray and the dice sets.

    python3 tools/make_meshes.py <BasicTankard01.nif> <png folder>

<BasicTankard01.nif>: any vanilla clutter mesh with one BSTriShape (Skyrim - Meshes0.bsa: meshes\\clutter\\...\\
BasicTankard01.nif). Only its header and the layout of its node, shape and shader blocks are used; every vertex,
triangle and texture is ours (the same approach Hearthsworn takes for its cards).
<png folder>: the preview's own textures, written by the preview's __yacht.exportTextures() (see README): linen,
wood, woodLong, rack, iron, and dice_<set> atlases (3 x 2 faces), each with a _h height map; dice_<set>_m is where
the die is metal.

Writes assets/meshes/SkyrimYacht/Tray.nif, Die_<set>.nif and assets/textures/SkyrimYacht/*.dds. Units are game units,
the same as the physics (src/sim/Dice.h), z up.
"""
import io, math, os, struct, sys

from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nif import Nif

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets")
TEXDIR = "textures\\SkyrimYacht\\"

# The physics' sizes (DiceSpec)
DIE, ROUND = 1.2, 0.14
X, YH = 12.65, 8.6
WALL_H, WALL_T = 1.7, 0.8
RACK_Y = -YH - WALL_T - 1.7

SETS = ["bone", "iron", "steel", "corundum", "dwemer", "orichalcum", "silver", "quicksilver", "moonstone", "gold",
        "malachite", "ebony", "stalhrim", "dragonbone", "daedric"]
# How shiny each set is: (specular strength, glossiness, uses the metal environment map)
LOOK = {"bone": (0.25, 18, False), "dragonbone": (0.35, 24, False), "malachite": (0.9, 80, False), "moonstone": (0.8, 70, True),
        "stalhrim": (1.0, 90, False)}
for s in SETS:
    LOOK.setdefault(s, (0.8, 60, True))


# ------------------------------------------------------------------ textures
def dds(path, img, fmt="DXT1"):
    """A block-compressed DDS (DXT1, or DXT5 with alpha) with mips down to 4 x 4 (as Hearthsworn writes them)."""
    img = img.convert("RGBA")
    levels = [img]
    while min(levels[-1].size) > 4:
        w, h = levels[-1].size
        levels.append(levels[-1].resize((w // 2, h // 2), Image.LANCZOS))
    body = b""
    for m in levels:
        buf = io.BytesIO()
        m.save(buf, format="DDS", pixel_format=fmt)
        body += buf.getvalue()[128:]
    w, h = img.size
    top = (w // 4) * (h // 4) * (8 if fmt == "DXT1" else 16)
    hdr = struct.pack("<4sIIIIIII11I", b"DDS ", 124, 0xA1007, h, w, top, 0, len(levels), *([0] * 11))
    hdr += struct.pack("<II4sIIIII", 32, 0x4, fmt.encode(), 0, 0, 0, 0, 0)
    hdr += struct.pack("<IIIII", 0x401008, 0, 0, 0, 0)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(hdr + body)


def normal_map(height, strength, spec):
    """A tangent-space normal map from a height map; alpha is the specular mask."""
    h = height.convert("L")
    w, hh = h.size
    px = h.load()
    out = Image.new("RGBA", (w, hh))
    op = out.load()
    sp = spec.convert("L").resize((w, hh)).load() if isinstance(spec, Image.Image) else None
    for y in range(hh):
        for x in range(w):
            dx = (px[(x + 1) % w, y] - px[(x - 1) % w, y]) / 255.0 * strength
            dy = (px[x, (y + 1) % hh] - px[x, (y - 1) % hh]) / 255.0 * strength
            n = (-dx, dy, 1.0)
            l = math.sqrt(n[0] ** 2 + n[1] ** 2 + 1.0)
            a = sp[x, y] if sp else int(spec)
            op[x, y] = (int((n[0] / l * 0.5 + 0.5) * 255), int((n[1] / l * 0.5 + 0.5) * 255), int((n[2] / l * 0.5 + 0.5) * 255), a)
    return out


def textures(pngs):
    out = os.path.join(ROOT, "textures", "SkyrimYacht")
    def load(n, size):
        return Image.open(os.path.join(pngs, n + ".png")).convert("RGBA").resize(size, Image.LANCZOS)
    # (size, bump strength, specular) per texture; the strength is how steep one step of the height map is
    tray = {"linen": ((2048, 1024), 1.2, 30), "wood": ((2048, 1024), 2.0, 50), "woodLong": ((2048, 256), 2.0, 50),
            "rack": ((1024, 256), 2.0, 50), "iron": ((256, 256), 2.0, 200)}
    for name, (size, strength, spec) in tray.items():
        dds(os.path.join(out, name + ".dds"), load(name, size), "DXT1")
        nsize = (size[0] // 2, size[1] // 2)
        dds(os.path.join(out, name + "_n.dds"), normal_map(load(name + "_h", nsize), strength, spec), "DXT5")
        print("texture", name)
    for s in SETS:
        dds(os.path.join(out, f"Dice_{s}.dds"), load(f"dice_{s}", (768, 512)), "DXT1")
        metal = load(f"dice_{s}_m", (384, 256)).convert("L")
        spec = metal.point(lambda v: 60 + v * 195 // 255) if LOOK[s][2] else 90
        dds(os.path.join(out, f"Dice_{s}_n.dds"), normal_map(load(f"dice_{s}_h", (384, 256)), 3.0, spec), "DXT5")
        if LOOK[s][2]:
            dds(os.path.join(out, f"Dice_{s}_m.dds"), load(f"dice_{s}_m", (384, 256)), "DXT1")
        print("texture dice", s)


# ------------------------------------------------------------------ geometry
def nb(c):
    return max(0, min(255, int(round((c + 1.0) * 127.5))))


def vert(p, uv, n, t, bt):
    return (struct.pack("<4f", p[0], p[1], p[2], bt[0]) + struct.pack("<e", uv[0]) + struct.pack("<e", uv[1]) +
            bytes([nb(n[0]), nb(n[1]), nb(n[2]), nb(bt[1])]) + bytes([nb(t[0]), nb(t[1]), nb(t[2]), nb(bt[2])]))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return tuple(c / l for c in v)


class Mesh:
    def __init__(self):
        self.V, self.I = [], []

    def quad(self, ps, uvs, n, t):
        """Four corners counter-clockwise seen from the normal n; t is the direction u grows in."""
        bt = cross(n, t)
        base = len(self.V)
        for p, uv in zip(ps, uvs):
            self.V.append((p, uv, n, t, bt))
        self.I += [base, base + 1, base + 2, base, base + 2, base + 3]

    def box(self, w, d, h, x, y, z):
        """A board with the preview's texture layout: the grain runs once along its length on every long face."""
        hx, hy, hz = w / 2, d / 2, h / 2
        longX = w >= d
        L, C = (w, d) if longX else (d, w)
        def uv(px, py, pz, face):
            along = ((px if longX else py) / L) + 0.5
            crs = ((py if longX else px) / C) + 0.5
            up = pz / h + 0.5
            if face == "end":
                return (crs, 1 - up)
            if face == "top":
                return (along, 1 - crs)
            return (along, 1 - up)
        faces = [((1, 0, 0), "x"), ((-1, 0, 0), "x"), ((0, 1, 0), "y"), ((0, -1, 0), "y"), ((0, 0, 1), "z"), ((0, 0, -1), "z")]
        for n, axis in faces:
            # two in-plane axes for this face, ordered so the quad winds counter-clockwise around n
            if axis == "x":
                a, b = (0, 1, 0), (0, 0, 1)
            elif axis == "y":
                a, b = (0, 0, 1), (1, 0, 0)
            else:
                a, b = (1, 0, 0), (0, 1, 0)
            if sum(cross(a, b)[k] * n[k] for k in range(3)) < 0:
                a, b = b, a
            c = tuple(n[k] * (hx, hy, hz)[k] for k in range(3))
            ea = tuple(a[k] * (hx, hy, hz)[k] for k in range(3))
            eb = tuple(b[k] * (hx, hy, hz)[k] for k in range(3))
            corners = [tuple(c[k] - ea[k] - eb[k] for k in range(3)), tuple(c[k] + ea[k] - eb[k] for k in range(3)),
                       tuple(c[k] + ea[k] + eb[k] for k in range(3)), tuple(c[k] - ea[k] + eb[k] for k in range(3))]
            nAlong = abs(n[0] if longX else n[1])
            kind = "end" if nAlong > 0.5 else "top" if abs(n[2]) > 0.5 else "side"
            uvs = [uv(*p, kind) for p in corners]
            # the tangent: the direction u grows along this face
            du = (uvs[1][0] - uvs[0][0], uvs[3][0] - uvs[0][0])
            t = norm(tuple(ea[k] * du[0] + eb[k] * du[1] for k in range(3)))
            self.quad([(p[0] + x, p[1] + y, p[2] + z) for p in corners], uvs, n, t)

    def rivet(self, x, y, z, n, r=0.11):
        """A round rivet head on a surface facing n."""
        n = norm(n)
        a = norm(cross(n, (0, 0, 1) if abs(n[2]) < 0.9 else (1, 0, 0)))
        b = cross(n, a)
        rings, segs = 3, 10
        pts = []
        for i in range(rings + 1):
            th = (math.pi / 2) * i / rings
            for j in range(segs):
                ph = 2 * math.pi * j / segs
                d = tuple(a[k] * math.cos(ph) * math.cos(th) + b[k] * math.sin(ph) * math.cos(th) + n[k] * math.sin(th) for k in range(3))
                pts.append(d)
        base = len(self.V)
        for d in pts:
            p = (x + d[0] * r, y + d[1] * r, z + d[2] * r)
            t = norm(cross(n, d)) if abs(sum(n[k] * d[k] for k in range(3))) < 0.99 else a
            self.V.append((p, (0.5 + d[0] * 0.3, 0.5 + d[1] * 0.3), d, t, cross(d, t)))
        for i in range(rings):
            for j in range(segs):
                p0 = base + i * segs + j
                p1 = base + i * segs + (j + 1) % segs
                p2 = base + (i + 1) * segs + (j + 1) % segs
                p3 = base + (i + 1) * segs + j
                self.I += [p0, p1, p2, p0, p2, p3]

    def bounds(self):
        ps = [v[0] for v in self.V]
        c = tuple((min(p[k] for p in ps) + max(p[k] for p in ps)) / 2 for k in range(3))
        r = max(math.sqrt(sum((p[k] - c[k]) ** 2 for k in range(3))) for p in ps)
        return c, r

    def data(self):
        return b"".join(vert(*v) for v in self.V), self.I


def tray_meshes():
    linen, wood, wlong, rack, iron = Mesh(), Mesh(), Mesh(), Mesh(), Mesh()
    linen.quad([(-X, -YH, 0.0), (X, -YH, 0.0), (X, YH, 0.0), (-X, YH, 0.0)], [(0, 1), (1, 1), (1, 0), (0, 0)], (0, 0, 1), (1, 0, 0))
    wood.box(X * 2 + WALL_T * 2, YH * 2 + WALL_T * 2, 0.5, 0, 0, -0.26)
    wlong.box(X * 2 + WALL_T * 2, WALL_T, WALL_H, 0, YH + WALL_T / 2, WALL_H / 2 - 0.2)
    wlong.box(X * 2 + WALL_T * 2, WALL_T, WALL_H, 0, -YH - WALL_T / 2, WALL_H / 2 - 0.2)
    wlong.box(WALL_T, YH * 2, WALL_H, X + WALL_T / 2, 0, WALL_H / 2 - 0.2)
    wlong.box(WALL_T, YH * 2, WALL_H, -X - WALL_T / 2, 0, WALL_H / 2 - 0.2)
    top, strap, sw, th = WALL_H - 0.2, 2.4, 0.5, 0.06
    for sx in (-1, 1):
        for sy in (-1, 1):
            cx, cy = sx * (X + WALL_T / 2), sy * (YH + WALL_T / 2)
            ey, ex = cy + sy * (WALL_T - sw) / 2, cx + sx * (WALL_T - sw) / 2
            iron.box(strap, sw, th, cx - sx * (strap / 2 - WALL_T / 2), ey, top + th / 2)
            iron.box(sw, strap, th, ex, cy - sy * (strap / 2 - WALL_T / 2), top + th / 2)
            bot = -0.51
            hh, zc, ox, oy = top + th - bot, (top + th + bot) / 2, sx * (X + WALL_T), sy * (YH + WALL_T)
            iron.box(sw + th, th, hh, ox - sx * (sw - th) / 2, oy + sy * th / 2, zc)
            iron.box(th, sw + th, hh, ox + sx * th / 2, oy - sy * (sw - th) / 2, zc)
            for k in (0.35, 1.2, 2.0):
                iron.rivet(cx - sx * k, ey, top + th, (0, 0, 1))
                iron.rivet(ex, cy - sy * k, top + th, (0, 0, 1))
            for z in (WALL_H * 0.6, -0.26):
                iron.rivet(ox - sx * sw / 2, oy + sy * th, z, (0, sy, 0))
                iron.rivet(ox + sx * th, oy - sy * sw / 2, z, (sx, 0, 0))
    x = -X + 3
    while x <= X - 3 + 1e-6:
        for sy in (-1, 1):
            iron.rivet(x, sy * (YH + WALL_T / 2), top, (0, 0, 1))
        x += 3.2
    # the two racks: the player's in front, the opponent's behind
    for side in (-1, 1):
        ry = RACK_Y * (1 if side < 0 else -1)
        rack.box(13.5, 2.4, 0.5, 0, ry, -0.25)
        rack.box(13.5, 0.25, 0.7, 0, ry + side * 1.15, 0.1)
        iron.box(13.6, 0.08, 0.18, 0, ry + side * 1.3, 0.32)
        for xx in (-6.2, 0, 6.2):
            iron.rivet(xx, ry + side * 1.34, 0.32, (0, side, 0))
    return [("Linen", linen, "linen", (0.15, 10, False)), ("Base", wood, "wood", (0.3, 20, False)),
            ("Walls", wlong, "woodLong", (0.3, 20, False)), ("Racks", rack, "rack", (0.3, 20, False)),
            ("Iron", iron, "iron", (1.0, 60, True))]


def die_mesh():
    """The physics' rounded cube, each face mapped to its cell of the 3 x 2 atlas (+x 1, -x 6, +y 2, -y 5, +z 3, -z 4)."""
    m = Mesh()
    hs, r = DIE / 2, DIE * ROUND
    e = hs - r
    N = 7
    cells = [((1, 0, 0), 0), ((-1, 0, 0), 1), ((0, 1, 0), 2), ((0, -1, 0), 3), ((0, 0, 1), 4), ((0, 0, -1), 5)]
    pad = 2.0 / 512  # keep a hair inside each cell so neighbours don't bleed in
    for n, cell in cells:
        if abs(n[0]) > 0.5:
            a, b = (0, 1, 0), (0, 0, 1)
        elif abs(n[1]) > 0.5:
            a, b = (0, 0, 1), (1, 0, 0)
        else:
            a, b = (1, 0, 0), (0, 1, 0)
        if sum(cross(a, b)[k] * n[k] for k in range(3)) < 0:
            a, b = b, a
        u0, v0 = (cell % 3) / 3.0, (cell // 3) / 2.0
        base = len(m.V)
        for i in range(N + 1):
            for j in range(N + 1):
                s, t = i / N * 2 - 1, j / N * 2 - 1
                p = tuple(n[k] * hs + a[k] * s * hs + b[k] * t * hs for k in range(3))
                c = tuple(max(-e, min(e, p[k])) for k in range(3))
                d = tuple(p[k] - c[k] for k in range(3))
                dl = math.sqrt(sum(x * x for x in d))
                if dl > 1e-9:
                    dn = tuple(x / dl for x in d)
                    p = tuple(c[k] + dn[k] * r for k in range(3))
                else:
                    dn = n
                uv = (u0 + pad + (s * 0.5 + 0.5) * (1 / 3.0 - 2 * pad), v0 + pad + (1 - (t * 0.5 + 0.5)) * (0.5 - 2 * pad))
                tt = norm(tuple(a[k] - dn[k] * sum(a[q] * dn[q] for q in range(3)) for k in range(3)))
                m.V.append((p, uv, dn, tt, cross(dn, tt)))
        for i in range(N):
            for j in range(N):
                p0 = base + i * (N + 1) + j
                p1 = base + (i + 1) * (N + 1) + j
                p2 = base + (i + 1) * (N + 1) + j + 1
                p3 = base + i * (N + 1) + j + 1
                m.I += [p0, p1, p2, p0, p2, p3]
    return m


# ------------------------------------------------------------------ NIF
def shader_block(template, texset_ref, spec, gloss, env):
    """BSLightingShaderProperty: environment-mapped (metal) or the default shader, from the template's layout."""
    b = bytearray(template)
    struct.pack_into("<I", b, 0, 1 if env else 0)
    sf1, sf2 = struct.unpack_from("<II", b, 16)
    sf1 = (sf1 | 0x80) if env else (sf1 & ~0x80)
    sf2 = (sf2 | 0x8000) if env else (sf2 & ~0x8000)
    struct.pack_into("<II", b, 16, sf1, sf2)
    struct.pack_into("<i", b, 40, texset_ref)
    struct.pack_into("<f", b, 72, float(gloss))
    struct.pack_into("<3f", b, 76, 1.0, 1.0, 1.0)
    struct.pack_into("<f", b, 88, float(spec))
    return bytes(b if env else b[:100])  # the environment map scale only exists for that shader


def texset_block(paths):
    paths = list(paths) + [""] * (9 - len(paths))
    return struct.pack("<I", 9) + b"".join(struct.pack("<I", len(p)) + p.encode() for p in paths)


def write_nif(template_path, out, root_name, shapes):
    """shapes: [(name, Mesh, [texture paths], (spec, gloss, env))]"""
    n = Nif(open(template_path, "rb").read())
    assert [n.type(i) for i in range(len(n.blocks))][:3] == ["BSFadeNode", "BSInvMarker", "BSXFlags"], "unexpected template"
    shape_t, shader_t, texset_t = n.blocks[6], n.blocks[7], n.blocks[8]
    vd, = struct.unpack_from("<Q", shape_t, 100)
    assert (vd & 0xF) * 4 == 28, "the template's vertices must be 28 bytes (position, uv, normal, tangent)"
    n.shorts = [b"SkyrimYacht\x00", b"\x00", b"make_meshes.py\x00"]
    n.strings = [root_name, "INV", "BSX"] + [s[0] for s in shapes]
    types = ["BSFadeNode", "BSInvMarker", "BSXFlags", "BSTriShape", "BSLightingShaderProperty", "BSShaderTextureSet"]
    n.types = types
    blocks, tidx = [], []
    child_refs = [3 + 3 * i for i in range(len(shapes))]
    root = (struct.pack("<iI2i", 0, 2, 1, 2) + struct.pack("<iI", -1, 0x0E) + struct.pack("<3f", 0, 0, 0) +
            struct.pack("<9f", 1, 0, 0, 0, 1, 0, 0, 0, 1) + struct.pack("<f", 1.0) + struct.pack("<i", -1) +
            struct.pack("<I", len(shapes)) + b"".join(struct.pack("<i", r) for r in child_refs) + struct.pack("<I", 0))
    blocks += [root, struct.pack("<i3Hf", 1, 0, 0, 0, 1.0), struct.pack("<iI", 2, 0)]
    tidx += [0, 1, 2]
    for k, (name, mesh, tex, (spec, gloss, env)) in enumerate(shapes):
        me = 3 + 3 * k
        head = bytearray(shape_t[:108])
        struct.pack_into("<i", head, 0, 3 + k)                      # name
        struct.pack_into("<I", head, 12, 0x0E)                      # flags
        struct.pack_into("<3f9ff", head, 16, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1.0)
        struct.pack_into("<i", head, 68, -1)                        # no collision
        c, r = mesh.bounds()
        struct.pack_into("<4f", head, 72, c[0], c[1], c[2], r)
        struct.pack_into("<iii", head, 88, -1, me + 1, -1)          # skin, shader, alpha
        verts, idx = mesh.data()
        assert len(mesh.V) < 65536
        data = verts + struct.pack(f"<{len(idx)}H", *idx)
        shape = bytes(head) + struct.pack("<HHI", len(idx) // 3, len(mesh.V), len(data)) + data + struct.pack("<I", 0)
        blocks += [shape, shader_block(shader_t, me + 2, spec, gloss, env), texset_block(tex)]
        tidx += [3, 4, 5]
    n.blocks = [bytearray(b) for b in blocks]
    n.tidx = tidx
    n.groups = []
    n.footer = struct.pack("<II", 1, 0)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(n.write())
    print("wrote", out, sum(len(s[1].V) for s in shapes), "vertices")


def main(template, pngs):
    if pngs != "-":
        textures(pngs)
    meshes = os.path.join(ROOT, "meshes", "SkyrimYacht")
    shapes = []
    for name, mesh, tex, look in tray_meshes():
        paths = [TEXDIR + tex + ".dds", TEXDIR + tex + "_n.dds"]
        if look[2]:
            paths += ["", "", "textures\\cubemaps\\ShinyDull_e.dds", TEXDIR + tex + ".dds"]
        shapes.append((name, mesh, paths, look))
    write_nif(template, os.path.join(meshes, "Tray.nif"), "SkyrimYachtTray", shapes)
    die = die_mesh()
    for s in SETS:
        spec, gloss, env = LOOK[s]
        paths = [TEXDIR + f"Dice_{s}.dds", TEXDIR + f"Dice_{s}_n.dds"]
        if env:
            paths += ["", "", "textures\\cubemaps\\ShinyDull_e.dds", TEXDIR + f"Dice_{s}_m.dds"]
        write_nif(template, os.path.join(meshes, f"Die_{s}.nif"), f"SkyrimYachtDie_{s}", [("Die", die, paths, (spec, gloss, env))])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
