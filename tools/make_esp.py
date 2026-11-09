#!/usr/bin/env python3
"""Generates SkyrimYacht.esp (ESL-flagged, no scripts) and src/plugin/EspIds.h.

Records:
  GLOB  YA_CanAfford   - set by the DLL: 1 while you carry at least the smallest wager
  FACT  YA_TableFaction - hidden; the DLL puts NPCs in it while they sit near a table, so the dialogue
                          condition is a plain game check
  MISC  YA_Tray        - the tray, shown through the game's inventory 3D preview while you play
  QUST  YA_Quest       - holds the dialogue
  DLBR/DIAL/INFO       - "Care for a Yacht game?"; the DLL watches which INFO plays and opens the game

Header: version 1.70, form version 44, so the plugin loads on every version from 1.5.97 up (1.6.1130+ writes 1.71,
which older games refuse; nothing here needs it).

Usage: python3 tools/make_esp.py
"""
import os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
M = 0x01000000

IDS = {
    "CanAfford": 0x800, "TableFaction": 0x801, "Tray": 0x802, "Quest": 0x803,
    "BranchPlay": 0x804, "TopicPlay": 0x805, "InfoPlay": 0x806, "InfoPoor": 0x807,
    "DiceShop": 0x808,  # 0x809-0x80C were the old "Got any dice for sale?" topic: don't reuse
}
# The dice sets innkeepers sell, as items in their shop (the DLL stocks them); bone is everyone's from the start
DICE = [("Iron", 50), ("Steel", 100), ("Corundum", 150), ("Dwemer", 200), ("Orichalcum", 250), ("Silver", 300),
        ("Quicksilver", 400), ("Moonstone", 500), ("Gold", 650), ("Malachite", 750), ("Ebony", 900), ("Stalhrim", 1100),
        ("Dragonbone", 1500), ("Daedric", 2500)]
for _k, (_name, _gold) in enumerate(DICE):
    IDS[f"Dice{_name}"] = 0x80D + _k
# Shops only show the goods their buy/sell list names: the DLL adds this keyword to the innkeepers' lists
IDS["VendorItemDice"] = 0x80D + len(DICE)
INNKEEPER = 0x0005091B  # Skyrim.esm JobInnkeeperFaction
MIN_BET = 25


def sub(tag, data):
    return tag.encode() + struct.pack("<H", len(data)) + data


def zstr(s):
    return s.encode("cp1252") + b"\0"


def u8(v): return struct.pack("<B", v)
def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def i32(v): return struct.pack("<i", v)
def f32(v): return struct.pack("<f", v)


def fid(name):
    return M | IDS[name]


def record(tag, formid, payload, flags=0):
    return tag.encode() + struct.pack("<IIIHHHH", len(payload), flags, formid, 0, 0, 44, 0) + payload


def group(label, payload, gtype=0):
    lab = label.encode() if isinstance(label, str) else label
    return b"GRUP" + struct.pack("<I4siHHI", 24 + len(payload), lab, gtype, 0, 0, 0) + payload


def ctda(func, value, p1=0, op=0):
    """func(p1) <op> value on the subject (the NPC). op: 0 ==, 1 !=, 2 >, 3 >=, 4 <, 5 <=."""
    return sub("CTDA", u8(op << 5) + b"\0\0\0" + f32(value) + u16(func) + u16(0) + u32(p1) + u32(0) + u32(0) + u32(0) + i32(-1))


GET_IN_FACTION, GET_GLOBAL, IS_CHILD = 71, 74, 365
COUNT = 0


def rec(tag, name, payload, flags=0):
    global COUNT
    COUNT += 1
    return record(tag, fid(name), payload, flags)


def info(name, edid, text, conds, flags, prev=None):
    # ENAM: flags u16 (0x1 Goodbye, 0x200 Force Subtitle) + reset hours u16
    p = sub("EDID", zstr(edid)) + sub("ENAM", u16(flags) + u16(0)) + sub("PNAM", u32(fid(prev) if prev else 0))  # previous INFO in the topic
    trdt = u32(7) + u32(50) + u32(0) + u8(1) + b"\0\0\0" + u32(0) + u8(0) + b"\0\0\0"  # Neutral 50, response 1
    p += sub("TRDT", trdt) + sub("NAM1", zstr(text)) + sub("NAM2", zstr("")) + sub("NAM3", zstr(""))
    for c in conds:
        p += c
    return rec("INFO", name, p)


def main():
    glob = rec("GLOB", "CanAfford", sub("EDID", zstr("YA_CanAfford")) + sub("FNAM", b"s") + sub("FLTV", f32(0.0)))
    # 0: you haven't played yet (no line), 1: nothing new to sell, 2: a set of dice for sale
    glob += rec("GLOB", "DiceShop", sub("EDID", zstr("YA_DiceShop")) + sub("FNAM", b"s") + sub("FLTV", f32(0.0)))
    faction = rec("FACT", "TableFaction", sub("EDID", zstr("YA_TableFaction")) + sub("DATA", u32(0x1)) + sub("RNAM", u32(0)))
    tray = rec("MISC", "Tray", sub("EDID", zstr("YA_Tray")) + sub("OBND", struct.pack("<6h", -16, -16, -1, 16, 16, 2)) +
               sub("FULL", zstr("Yacht Tray")) + sub("MODL", zstr("SkyrimYacht\\Tray.nif")) + sub("DATA", i32(0) + f32(0.0)))
    # Quest: Start Game Enabled (0x1) | Has Dialogue Data (0x8000), priority 50, type side (8)
    quest = rec("QUST", "Quest", sub("EDID", zstr("YA_Quest")) + sub("FULL", zstr("Yacht")) +
                sub("DNAM", u16(0x0001 | 0x8000) + u8(50) + u8(0) + u32(0) + u32(8)) + sub("ANAM", u32(0)))
    branch = rec("DLBR", "BranchPlay", sub("EDID", zstr("YA_BranchPlay")) + sub("QNAM", u32(fid("Quest"))) + sub("TNAM", u32(0)) +
                 sub("DNAM", u32(0x1)) + sub("SNAM", u32(fid("TopicPlay"))))
    seated = [ctda(GET_IN_FACTION, 1.0, fid("TableFaction")), ctda(IS_CHILD, 0.0)]
    infos = (info("InfoPlay", "YA_InfoPlay", "Pull up a chair. Let's see your coin.",
                  seated + [ctda(GET_GLOBAL, 1.0, fid("CanAfford"))], 0x0201) +
             info("InfoPoor", "YA_InfoPoor", f"Come back when you've got the coin. {MIN_BET} septims, at least.",
                  seated + [ctda(GET_GLOBAL, 0.0, fid("CanAfford"))], 0x0201, prev="InfoPlay"))
    dial = rec("DIAL", "TopicPlay", sub("EDID", zstr("YA_TopicPlay")) + sub("FULL", zstr("Care for a Yacht game?")) +
               sub("PNAM", f32(50.0)) + sub("BNAM", u32(fid("BranchPlay"))) + sub("QNAM", u32(fid("Quest"))) +
               sub("DATA", u8(0) + u8(0) + u16(0)) + sub("SNAM", b"CUST") + sub("TIFC", u32(2)))
    global COUNT
    COUNT += 1  # the topic-children group
    dial += group(struct.pack("<I", fid("TopicPlay")), infos, gtype=7)
    # The dice sets, sold by innkeepers in their own shop (Buy or Sell)
    misc = tray
    for name, gold in DICE:
        misc += rec("MISC", f"Dice{name}", sub("EDID", zstr(f"YA_Dice{name}")) + sub("OBND", struct.pack("<6h", -1, -1, -1, 1, 1, 1)) +
                    sub("FULL", zstr(f"{name} Dice")) + sub("MODL", zstr(f"SkyrimYacht\\Die_{name.lower()}.nif")) +
                    sub("KSIZ", u32(1)) + sub("KWDA", u32(fid("VendorItemDice"))) + sub("DATA", i32(gold) + f32(0.5)))
    kywd = rec("KYWD", "VendorItemDice", sub("EDID", zstr("YA_VendorItemDice")))

    body = (group("KYWD", kywd) + group("GLOB", glob) + group("FACT", faction) + group("MISC", misc) + group("DIAL", dial) +
            group("QUST", quest) + group("DLBR", branch))
    groups = 7
    hedr = struct.pack("<fiI", 1.70, COUNT + groups, max(IDS.values()) + 1)
    tes4 = (sub("HEDR", hedr) + sub("CNAM", zstr("evodube")) +
            sub("SNAM", zstr("Yacht - a dice game you play with seated NPCs (needs SkyrimYacht.dll)")) +
            sub("MAST", zstr("Skyrim.esm")) + sub("DATA", b"\0" * 8))
    esp = record("TES4", 0, tes4, flags=0x200) + body  # 0x200 = ESL (light plugin)
    open(os.path.join(ROOT, "SkyrimYacht.esp"), "wb").write(esp)
    # Data/Seq/SkyrimYacht.seq: the Start Game Enabled quests with dialogue (what the Creation Kit's "Generate SEQ File"
    # writes), so the game loads the quest's dialogue when it starts the quest itself.
    os.makedirs(os.path.join(ROOT, "assets", "seq"), exist_ok=True)
    open(os.path.join(ROOT, "assets", "seq", "SkyrimYacht.seq"), "wb").write(struct.pack("<I", fid("Quest")))

    L = ["#pragma once", "// Generated by tools/make_esp.py - keep in sync with SkyrimYacht.esp.", "",
         "namespace EspIds", "{", '\tinline constexpr const char* kPlugin = "SkyrimYacht.esp";']
    for k, v in IDS.items():
        L.append(f"\tinline constexpr RE::FormID k{k} = 0x{v:X};")
    L += [f"\tinline constexpr int kMinBet = {MIN_BET};", "}", ""]
    open(os.path.join(ROOT, "src", "plugin", "EspIds.h"), "w").write("\n".join(L))
    print(f"wrote SkyrimYacht.esp ({len(esp)} bytes, {COUNT} records) and src/plugin/EspIds.h")


if __name__ == "__main__":
    main()
