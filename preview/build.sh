#!/usr/bin/env bash
# Builds the dice simulation to WebAssembly and bakes it into preview/yacht-preview.html.
# Needs clang with the wasm32 target and wasm-ld (LLVM 15+).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
clang++ --target=wasm32 -std=c++20 -O2 -nostdlib -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -fvisibility=hidden -Wall -Wextra -Wl,--no-entry -Wl,--export-dynamic -Wl,--export=__wasm_call_ctors \
  -o build/yacht.wasm preview/api.cpp src/sim/Dice.cpp src/sim/Yacht.cpp
python3 - <<'PY'
import base64
wasm = base64.b64encode(open("build/yacht.wasm", "rb").read()).decode()
page = open("preview/template.html").read().replace("__WASM_BASE64__", wasm)
open("preview/yacht-preview.html", "w").write(page)
print("preview/yacht-preview.html", len(page), "bytes")
PY
