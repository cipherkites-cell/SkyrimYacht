#!/usr/bin/env bash
# Builds the dice simulation to WebAssembly and bakes it into preview/yacht-preview.html.
# Needs clang with the wasm32 target and wasm-ld (LLVM 15+).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
clang++ --target=wasm32 -std=c++20 -O2 -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -fvisibility=hidden -Wall -Wextra -Wl,--no-entry -Wl,--export-dynamic -Wl,--export=__wasm_call_ctors \
  -o build/yacht.wasm preview/api.cpp src/sim/Dice.cpp src/sim/Yacht.cpp src/sim/Ai.cpp
python3 - <<'PY'
import base64
wasm = base64.b64encode(open("build/yacht.wasm", "rb").read()).decode()
import os
page = open("preview/template.html").read().replace("__WASM_BASE64__", wasm)
# Real dice hits (tools/make_dice_hits.py, from CC0 recordings).
import json
hits = {"ogg": base64.b64encode(open("assets/sounds/dice_hits.ogg", "rb").read()).decode(),
        "index": json.load(open("assets/sounds/dice_hits.json"))}
page = page.replace("/*__DICE_HITS__*/null", json.dumps(hits))
if os.environ.get("LOCAL") == "1":
    # The game's own cups (tools/make_local_cups.py). Bethesda's assets: this page stays on your own machine.
    cups = open("local-assets/cups.js").read()
    page = page.replace("<!--__LOCAL_CUPS__-->", "<script>" + cups + "</script>")
    open("preview/yacht-local.html", "w").write(page)
    print("preview/yacht-local.html", len(page), "bytes")
else:
    open("preview/yacht-preview.html", "w").write(page)
    print("preview/yacht-preview.html", len(page), "bytes")
PY
