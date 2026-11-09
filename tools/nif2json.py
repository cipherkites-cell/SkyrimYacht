#!/usr/bin/env python3
"""Reads the triangle meshes out of a Skyrim SE .nif (BSTriShape, version 20.2.0.7 / BS 100) into JSON.

Used to put the game's own cups into a local copy of the browser preview. Only what that needs is parsed: the node
tree (for transforms), BSTriShape geometry and each shape's texture set.

    python3 tools/nif2json.py in.nif out.json
"""
import json
import math
import struct
import sys


class Reader:
    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos

    def u8(self):
        v = self.d[self.p]
        self.p += 1
        return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]
        self.p += 2
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from("<Q", self.d, self.p)[0]
        self.p += 8
        return v

    def f32(self, n=1):
        v = struct.unpack_from("<%df" % n, self.d, self.p)
        self.p += 4 * n
        return v if n > 1 else v[0]

    def bytes(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def sstr(self):  # byte-length string (header)
        return self.bytes(self.u8()).rstrip(b"\0").decode("latin-1")

    def lstr(self):  # uint-length string
        return self.bytes(self.u32()).decode("latin-1")


def half(h):
    return struct.unpack("<e", struct.pack("<H", h))[0]


def read_header(r):
    line_end = r.d.index(b"\n")
    r.p = line_end + 1
    version = r.u32()
    assert version == 0x14020007, "not a Skyrim SE nif"
    r.u8()  # endian
    user_version = r.u32()
    num_blocks = r.u32()
    bs_version = r.u32()
    r.sstr()  # author
    if bs_version > 130:
        r.u32()
    r.sstr()  # process script
    r.sstr()  # export script
    if bs_version == 130:
        r.sstr()
    types = [r.lstr() for _ in range(r.u16())]
    type_of = [types[r.u16() & 0x7FFF] for _ in range(num_blocks)]
    sizes = [r.u32() for _ in range(num_blocks)]
    num_strings = r.u32()
    r.u32()  # max length
    strings = [r.lstr() for _ in range(num_strings)]
    for _ in range(r.u32()):
        r.u32()
    return type_of, sizes, strings, bs_version


def read_avobject(r):
    name = r.i32()
    r.bytes(4 * r.u32())  # extra data refs
    r.i32()  # controller
    r.u32()  # flags
    t = r.f32(3)
    rot = r.f32(9)  # row-major 3x3
    s = r.f32()
    r.i32()  # collision
    return name, (t, rot, s)


def compose(a, b):
    """World transform of b inside a. Each is (t, rot rows, scale)."""
    ta, ra, sa = a
    tb, rb, sb = b
    rot = [sum(ra[i * 3 + k] * rb[k * 3 + j] for k in range(3)) for i in range(3) for j in range(3)]
    t = [ta[i] + sa * sum(ra[i * 3 + k] * tb[k] for k in range(3)) for i in range(3)]
    return t, rot, sa * sb


def apply(xf, v, normal=False):
    t, rot, s = xf
    out = [sum(rot[i * 3 + k] * v[k] for k in range(3)) for i in range(3)]
    if normal:
        return out
    return [t[i] + s * out[i] for i in range(3)]


def main(src, dst):
    data = open(src, "rb").read()
    r = Reader(data)
    type_of, sizes, strings, bsver = read_header(r)
    starts = []
    p = r.p
    for size in sizes:
        starts.append(p)
        p += size

    parent = {}
    local = {}
    shapes = []
    for i, ty in enumerate(type_of):
        br = Reader(data, starts[i])
        if ty in ("NiNode", "BSFadeNode", "BSLeafAnimNode", "BSMultiBoundNode"):
            _, xf = read_avobject(br)
            local[i] = xf
            for _ in range(br.u32()):
                c = br.i32()
                if c >= 0:
                    parent[c] = i
        elif ty == "BSTriShape":
            name, xf = read_avobject(br)
            local[i] = xf
            br.f32(4)  # bounding sphere
            br.i32()  # skin
            shader = br.i32()
            br.i32()  # alpha
            desc = br.u64()
            ntri = br.u16() if bsver < 130 else br.u32()
            nvert = br.u16()
            br.u32()  # data size
            # BSVertexDesc: 4 bits each for vertex size, dynamic size, UV1, UV2, normal, tangent... (in 4-byte units),
            # then the attribute flags from bit 44.
            vsize = (desc & 0xF) * 4
            uv_off = ((desc >> 8) & 0xF) * 4
            n_off = ((desc >> 16) & 0xF) * 4
            flags = desc >> 44
            has_uv = bool(flags & (1 << 1))
            has_n = bool(flags & (1 << 3))
            verts, norms, uvs = [], [], []
            base = br.p
            for v in range(nvert):
                o = base + v * vsize
                verts.append(list(struct.unpack_from("<3f", data, o)))
                if has_uv:
                    u, w = struct.unpack_from("<HH", data, o + uv_off)
                    uvs.append([half(u), half(w)])
                if has_n:
                    nx, ny, nz = struct.unpack_from("<3B", data, o + n_off)
                    norms.append([nx / 255 * 2 - 1, ny / 255 * 2 - 1, nz / 255 * 2 - 1])
            br.p = base + nvert * vsize
            tris = [list(struct.unpack_from("<3H", data, br.p + t * 6)) for t in range(ntri)]
            shapes.append({"block": i, "name": strings[name] if 0 <= name < len(strings) else "", "shader": shader,
                           "verts": verts, "norms": norms, "uvs": uvs, "tris": tris})

    # Texture sets: BSLightingShaderProperty -> BSShaderTextureSet. The texture set ref sits after the shader's
    # NiObjectNET part and its flags/UV fields; simplest is to scan the shader block for a ref to a texture set.
    texsets = {}
    for i, ty in enumerate(type_of):
        if ty == "BSShaderTextureSet":
            br = Reader(data, starts[i])
            texsets[i] = [br.lstr() for _ in range(br.i32())]
    out = []
    for sh in shapes:
        textures = []
        if sh["shader"] >= 0:
            s = starts[sh["shader"]]
            for off in range(0, sizes[sh["shader"]] - 3, 1):
                ref = struct.unpack_from("<i", data, s + off)[0]
                if ref in texsets:
                    textures = texsets[ref]
                    break
        # World transform up the node tree.
        xf = local[sh["block"]]
        node = parent.get(sh["block"])
        while node is not None:
            xf = compose(local[node], xf)
            node = parent.get(node)
        verts = [apply(xf, v) for v in sh["verts"]]
        norms = []
        for n in sh["norms"]:
            w = apply(xf, n, normal=True)
            l = math.sqrt(sum(c * c for c in w)) or 1.0
            norms.append([c / l for c in w])
        out.append({"name": sh["name"], "textures": textures, "positions": [c for v in verts for c in v],
                    "normals": [c for n in norms for c in n], "uvs": [c for u in sh["uvs"] for c in u],
                    "indices": [c for t in sh["tris"] for c in t]})
    json.dump({"shapes": out}, open(dst, "w"))
    allv = [out_s["positions"][k:k + 3] for out_s in out for k in range(0, len(out_s["positions"]), 3)]
    lo = [min(v[i] for v in allv) for i in range(3)]
    hi = [max(v[i] for v in allv) for i in range(3)]
    print(f"{src}: {len(out)} shape(s), {len(allv)} verts, bounds {[round(x, 2) for x in lo]} .. {[round(x, 2) for x in hi]}")
    for s in out:
        print("   ", s["name"], len(s["indices"]) // 3, "tris", s["textures"][:2])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
