#!/usr/bin/env python3
"""The game's sounds, baked to WAV for the SKSE plugin (Data/SKSE/Plugins/SkyrimYacht/Sounds).

The browser preview builds its sounds live with Web Audio; the plugin plays these through XAudio2 and does the
per-hit parts itself (pitch, volume, a low-pass, pan). Everything here matches the preview:
  - click_N / knock_N: the single dice hits cut from the recordings (assets/sounds/dice_hits.ogg), plain, and with
    the metal and precious-metal tone (low shelf up, a little clank at 2.1 kHz, the fizz on top taken off)
  - clang_<cup>_N: each cup's short metal clang while shaking (four takes per cup)
  - roll: the soft scuff of dice sliding on the linen (looped)
  - shout: Fus Ro Dah, rendered by the preview itself (pass --shout path.wav from tools/render_shout.js)
  - ui_hover / ui_select / ui_equip / ui_deny, quill, swoosh
All mono, 16-bit, 48 kHz.
"""
import json
import math
import os
import subprocess
import sys
import wave

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "game_sounds")
SR = 48000
rng = np.random.default_rng(7)


def write(name, x):
    x = np.asarray(x, dtype=np.float64)
    x = np.clip(x, -1.0, 1.0)
    data = (x * 32767.0).astype("<i2").tobytes()
    with wave.open(os.path.join(OUT, name + ".wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(data)


# ---- RBJ biquads, as Web Audio's BiquadFilterNode
def biquad(x, kind, f0, q=0.7071, gain_db=0.0):
    A = 10 ** (gain_db / 40.0)
    w0 = 2 * math.pi * f0 / SR
    cw, sw = math.cos(w0), math.sin(w0)
    if kind in ("lowpass", "highpass", "bandpass"):
        alpha = sw / (2 * q)
    elif kind == "peaking":
        alpha = sw / (2 * q)
    else:  # shelves (Web Audio uses S = 1)
        alpha = sw / 2 * math.sqrt((A + 1 / A) * (1 / 1.0 - 1) + 2)
    if kind == "lowpass":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]; a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "highpass":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]; a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "bandpass":
        b = [alpha, 0, -alpha]; a = [1 + alpha, -2 * cw, 1 - alpha]
    elif kind == "peaking":
        b = [1 + alpha * A, -2 * cw, 1 - alpha * A]; a = [1 + alpha / A, -2 * cw, 1 - alpha / A]
    elif kind == "lowshelf":
        sa = 2 * math.sqrt(A) * alpha
        b = [A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa)]
        a = [(A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa]
    elif kind == "highshelf":
        sa = 2 * math.sqrt(A) * alpha
        b = [A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa)]
        a = [(A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa]
    b = [v / a[0] for v in b]; a = [1.0, a[1] / a[0], a[2] / a[0]]
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a[1] * y1 - a[2] * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def decode_hits():
    src = os.path.join(ROOT, "assets", "sounds", "dice_hits.ogg")
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-f", "f32le", "-ac", "1", "-ar", str(SR), "-"],
                         check=True, capture_output=True).stdout
    buf = np.frombuffer(raw, dtype="<f4").astype(np.float64)
    index = json.load(open(os.path.join(ROOT, "assets", "sounds", "dice_hits.json")))
    cut = lambda se: buf[int(round(se[0] * SR)): int(round(se[0] * SR)) + int(round(se[1] * SR))]
    return {k: [cut(se) for se in v] for k, v in index.items()}


# the preview's METAL_TONE
TONES = {"metal": (5, 2.5, -7), "precious": (6, 2, -8)}


def tone(x, t):
    low, clank, top = TONES[t]
    pad = np.concatenate([x, np.zeros(int(0.02 * SR))])  # room for the filters' tails
    y = biquad(pad, "lowshelf", 260, gain_db=low)
    y = biquad(y, "peaking", 2100, q=1.4, gain_db=clank)
    y = biquad(y, "highshelf", 5500, gain_db=top)
    return y


# the preview's CUP_CLANG
CUP_CLANG = {
    "tankard": [(520, 0.05), (1230, 0.035), (2140, 0.024)],
    "goblet": [(690, 0.045), (1640, 0.03), (2870, 0.02)],
    "dwemer": [(430, 0.065), (1070, 0.045), (1880, 0.03)],
    "silver": [(780, 0.055), (1860, 0.038), (3150, 0.024)],
    "golden": [(600, 0.075), (1440, 0.05), (2560, 0.032)],
}


def clang(modes):
    n = int(math.ceil(max(m[1] for m in modes) * 4 * SR))
    o = np.zeros(n)
    i = np.arange(n)
    for k, (f0, dec) in enumerate(modes):
        for det in (1.0, 1.002 + rng.random() * 0.002):
            w = 2 * math.pi * f0 * det * (1 + (rng.random() - 0.5) * 0.03) / SR
            kk = math.exp(-1 / (dec * SR))
            ph = rng.random() * 6.283
            env = 0.5 / (k + 1) * kk ** i
            o += env * np.sin(w * i + ph) * np.minimum(1, i / 96)
    o *= 0.9 / np.max(np.abs(o))
    fade = int(n * 0.25)
    o[n - fade:] *= np.linspace(1, 0, fade)
    return biquad(o, "lowpass", 3600)  # no glassy top, as in the preview


def noise(sec):
    return rng.random(int(sec * SR)) * 2 - 1


def env_exp(n, a, b, t_end):
    """exponentialRampToValueAtTime from a to b over t_end seconds."""
    t = np.arange(n) / SR
    return a * (b / a) ** np.minimum(1, t / t_end)


def ui_sounds():
    # hover: a light tick
    n = int(0.04 * SR)
    write("ui_hover", biquad(noise(0.04), "bandpass", 2600, q=2) * env_exp(n, 0.05, 0.0001, 0.03) * 2.2)
    # deny: a low buzz
    n = int(0.18 * SR); t = np.arange(n) / SR
    f = 140 * (90 / 140) ** np.minimum(1, t / 0.14)
    ph = 2 * np.pi * np.cumsum(f) / SR
    tri = 2 / np.pi * np.arcsin(np.sin(ph))
    write("ui_deny", tri * env_exp(n, 0.12, 0.0001, 0.16) * 2.2)
    # select / equip: a muffled wooden thunk (equip also knocks the tray in the game)
    for name, f0 in (("ui_select", 190), ("ui_equip", 240)):
        n = int(0.12 * SR); t = np.arange(n) / SR
        f = f0 * (90 / f0) ** np.minimum(1, t / 0.08)
        s = np.sin(2 * np.pi * np.cumsum(f) / SR) * env_exp(n, 0.22, 0.0001, 0.1)
        if name == "ui_select":
            m = int(0.05 * SR)
            s[:m] += biquad(noise(0.05), "bandpass", 1800, q=1.4) * env_exp(m, 0.09, 0.0001, 0.04)
        write(name, s * 2.2)


def quill():
    out = np.zeros(int(0.7 * SR))
    hp = biquad(noise(0.7), "highpass", 1500)
    t = 0.0
    for _ in range(4 + int(rng.random() * 3)):
        ln = 0.05 + rng.random() * 0.07
        a, b = int(t * SR), int((t + ln) * SR)
        if b >= len(out):
            break
        seg = biquad(hp[a:b], "bandpass", 3000 + rng.random() * 2200, q=1.2)
        g = np.interp(np.arange(b - a) / SR, [0, 0.012, ln * 0.6, ln], [0, 0.1 + rng.random() * 0.06, 0.03, 0])
        out[a:b] += seg * g
        t += ln + 0.02 + rng.random() * 0.04
    write("quill", out * 2.4)


def swoosh():
    n = int(0.45 * SR); t = np.arange(n) / SR
    x = noise(0.45)
    # a band sweeping 500 -> 1700 -> 600 Hz, done in short blocks
    f = np.where(t < 0.16, 500 * (1700 / 500) ** (t / 0.16), 1700 * (600 / 1700) ** np.minimum(1, (t - 0.16) / 0.22))
    y = np.zeros(n)
    blk = 480
    for s in range(0, n, blk):
        y[s:s + blk] = biquad(x[max(0, s - 2000):s + blk], "bandpass", float(f[s]), q=1.1)[-len(y[s:s + blk]):]
    g = np.where(t < 0.12, 0.0001 * (1 / 0.0001) ** (t / 0.12), (0.0001) ** np.minimum(1, (t - 0.12) / 0.3))
    write("swoosh", y * g * 0.9)


def coins():
    # a small handful of coins set down: a few bright, short metal clinks
    out = np.zeros(int(0.5 * SR))
    t0 = 0.0
    for k in range(5):
        n = int(0.12 * SR)
        i = np.arange(n)
        c = np.zeros(n)
        for f0, dec, a in ((3150, 0.05, 1.0), (5230, 0.035, 0.6), (7820, 0.02, 0.35)):
            f = f0 * (1 + (rng.random() - 0.5) * 0.08)
            c += a * np.exp(-i / (dec * SR)) * np.sin(2 * np.pi * f * i / SR + rng.random() * 6.28)
        c *= np.minimum(1, i / 24)
        a0 = int(t0 * SR)
        out[a0:a0 + n] += c[: len(out) - a0] * (0.9 - k * 0.12)
        t0 += 0.045 + rng.random() * 0.05
    write("coins", out / np.max(np.abs(out)) * 0.5)


def roll():
    # the scuff bed: band-passed noise, looped seamlessly (crossfaded ends)
    x = noise(2.2)
    y = biquad(biquad(x, "bandpass", 900, q=0.6), "lowpass", 2600)
    n = int(2.0 * SR); xf = int(0.2 * SR)
    loop = y[:n].copy()
    loop[:xf] = loop[:xf] * np.linspace(0, 1, xf) + y[n:n + xf] * np.linspace(1, 0, xf)
    write("roll", loop / np.max(np.abs(loop)) * 0.9)


def main():
    os.makedirs(OUT, exist_ok=True)
    hits = decode_hits()
    for kind in ("click", "knock"):
        for i, h in enumerate(hits[kind]):
            write(f"{kind}_{i}", np.concatenate([h, np.zeros(int(0.01 * SR))]))
            for t in TONES:
                write(f"{kind}_{t}_{i}", tone(h, t))
    for cup, modes in CUP_CLANG.items():
        for v in range(4):
            write(f"clang_{cup}_{v}", clang(modes))
    ui_sounds()
    quill()
    swoosh()
    roll()
    coins()
    if "--shout" in sys.argv:
        src = sys.argv[sys.argv.index("--shout") + 1]
        x = np.array(json.load(open(src)), dtype=np.float64)
        write("shout", x)
    json.dump({k: len(v) for k, v in hits.items()}, open(os.path.join(OUT, "index.json"), "w"))
    print("wrote", len(os.listdir(OUT)), "files to", OUT)


if __name__ == "__main__":
    main()
