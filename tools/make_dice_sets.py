#!/usr/bin/env python3
"""The dice sets as items: six dice in a small heap, for the shop, your inventory and the ground.

    python3 tools/make_dice_sets.py

Reads assets/meshes/SkyrimYacht/Die_<set>.nif (from make_meshes.py) and writes DiceSet_<set>.nif beside it: the same
shader and textures, the one die's mesh copied six times, each turned to show a different face.
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nif import Nif
from make_meshes import SETS, DIE, Mesh, die_mesh

MESHES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "assets", "meshes", "SkyrimYacht")


def rot(axis, deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    if axis == "x":
        return ((1, 0, 0), (0, c, -s), (0, s, c))
    if axis == "y":
        return ((c, 0, s), (0, 1, 0), (-s, 0, c))
    return ((c, -s, 0), (s, c, 0), (0, 0, 1))


def mul(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def ap(m, v):
    return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))


# (x, y, z, which face up, yaw): +z shows 3, so turn the die to bring 1, 6, 2, 5 or 4 up
FACE_UP = {3: rot("x", 0), 4: rot("x", 180), 1: rot("y", -90), 6: rot("y", 90), 2: rot("x", 90), 5: rot("x", -90)}
H = DIE / 2
HEAP = [(0.0, 1.0, H, 5, 12), (-0.87, -0.5, H, 3, -20), (0.87, -0.5, H, 6, 38),
        (0.0, 0.05, H + DIE, 1, 24), (2.25, 0.55, H, 2, 64), (-2.2, 0.75, H, 4, -48)]


def heap():
    die = die_mesh()
    m = Mesh()
    for x, y, z, face, yaw in HEAP:
        r = mul(rot("z", yaw), FACE_UP[face])
        base = len(m.V)
        for p, uv, n, t, b in die.V:
            q = ap(r, p)
            m.V.append(((q[0] + x, q[1] + y, q[2] + z), uv, ap(r, n), ap(r, t), ap(r, b)))
        m.I += [base + i for i in die.I]
    return m


def main():
    mesh = heap()
    verts, idx = mesh.data()
    data = verts + struct.pack(f"<{len(idx)}H", *idx)
    c, r = mesh.bounds()
    for s in SETS:
        n = Nif(open(os.path.join(MESHES, f"Die_{s}.nif"), "rb").read())
        assert n.type(3) == "BSTriShape"
        head = bytearray(n.blocks[3][:108])
        struct.pack_into("<4f", head, 72, c[0], c[1], c[2], r)
        n.blocks[3] = bytearray(bytes(head) + struct.pack("<HHI", len(idx) // 3, len(mesh.V), len(data)) + data + struct.pack("<I", 0))
        n.strings[0] = f"SkyrimYachtDiceSet_{s}"
        open(os.path.join(MESHES, f"DiceSet_{s}.nif"), "wb").write(n.write())
        print("wrote", f"DiceSet_{s}.nif", len(mesh.V), "vertices")


if __name__ == "__main__":
    main()
