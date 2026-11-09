#!/usr/bin/env python3
"""Cuts single dice hits out of real recordings for the preview's sound.

Sources (assets/sounds/kenney, CC0, by Kenney Vleugels, kenney.nl): dieShuffle1 is dice rattling in a cup, which gives
sharp "click" hits; dieThrow1/2 and dice-throw-1..3 are dice thrown onto a table, which give "knock" hits with the
table's thump. Each hit is cut from its onset, given a short fade, and written one after another into
assets/sounds/dice_hits.ogg, with assets/sounds/dice_hits.json listing where each starts and ends.
Needs ffmpeg and numpy.
"""
import json, os, subprocess, sys
import numpy as np

SR = 48000
ROOT = os.path.join(os.path.dirname(__file__), "..")
SRC = os.path.join(ROOT, "assets", "sounds", "kenney")

def load(name):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", os.path.join(SRC, name), "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).copy()

def onsets(x, rel, gap):
    env = np.convolve(np.abs(x), np.ones(24) / 24, "same")
    thr = env.max() * rel
    out, i = [], 0
    while i < len(x):
        if env[i] > thr:
            j = max(0, i - 48)
            while j > 0 and env[j] > thr * 0.15:  # back up to where it starts
                j -= 1
            out.append(j)
            i += gap
        else:
            i += 1
    return out

def cut(x, at, length, fade):
    # One hit only: the attack must hold the slice's loudest moment (a later, louder spike is a second bounce).
    whole = np.abs(x[at:at + length])
    if whole[:int(SR * 0.004)].max() < whole.max() * 0.7:
        return None
    seg = x[at:at + length].copy()
    if len(seg) < length:
        seg = np.pad(seg, (0, length - len(seg)))
    seg[-fade:] *= np.linspace(1, 0, fade) ** 2
    seg[:24] *= np.linspace(0, 1, 24)
    peak = np.abs(seg).max()
    return seg * (0.9 / peak) if peak > 0 else seg

hits = {"click": [], "knock": []}
x = load("dieShuffle1.ogg")
for at in onsets(x, 0.18, int(SR * 0.03)):
    hits["click"].append(cut(x, at, int(SR * 0.009), int(SR * 0.004)))
for name in ["dieThrow1.ogg", "dieThrow2.ogg", "dice-throw-1.ogg", "dice-throw-2.ogg", "dice-throw-3.ogg"]:
    x = load(name)
    for at in onsets(x, 0.2, int(SR * 0.018)):
        hits["knock"].append(cut(x, at, int(SR * 0.028), int(SR * 0.014)))

gap = np.zeros(int(SR * 0.02), dtype=np.float32)
parts, index, pos = [], {"click": [], "knock": []}, 0
for kind, segs in hits.items():
    for s in [s for s in segs if s is not None]:
        parts += [s.astype(np.float32), gap]
        index[kind].append([round(pos / SR, 5), round(len(s) / SR, 5)])
        pos += len(s) + len(gap)
pcm = np.concatenate(parts)
out = os.path.join(ROOT, "assets", "sounds", "dice_hits.ogg")
subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "f32le", "-ar", str(SR), "-ac", "1", "-i", "-", "-c:a", "libvorbis", "-q:a", "6", out],
               input=pcm.tobytes(), check=True)
json.dump(index, open(os.path.join(ROOT, "assets", "sounds", "dice_hits.json"), "w"))
print({k: len(v) for k, v in index.items()}, os.path.getsize(out), "bytes")
