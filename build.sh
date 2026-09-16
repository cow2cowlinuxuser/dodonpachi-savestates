#!/usr/bin/env bash
# Portable build for non-Windows hosts. Mirrors build.ps1 target-for-target,
# using zig as the C compiler exactly as the Windows script does: zig
# cross-compiles to Windows from any host, so the same PE32 d3d9.dll and the
# same test executables come out on Linux or macOS as on Windows.
set -euo pipefail
cd "$(dirname "$0")"

# zig is only used as a clang driver; nothing here needs zig's build system.
ZIG="${ZIG:-$(command -v zig || true)}"
if [ -z "$ZIG" ] && [ -x /opt/zig/zig ]; then ZIG=/opt/zig/zig; fi
if [ -z "$ZIG" ]; then
  echo "zig not found. Install from https://ziglang.org/download/ and put it on PATH, or set \$ZIG." >&2
  exit 1
fi

echo "using $ZIG (zig $("$ZIG" version))"

WARN=(-O2 -Wall -Wno-incompatible-function-pointer-types)
SRC=(src/d3d9_sw.c src/swrast.c src/savestate.c src/vsinterp.c \
     src/trace.c src/tramp.c src/allocwatch.c src/d3d9.def)

mkdir -p build

# The game is a 32-bit process, so this is the one that actually ships.
"$ZIG" cc "${WARN[@]}" -Isrc -DD3D9SW_VARIANT=stock -target x86-windows-gnu \
  -shared -o build/d3d9.dll "${SRC[@]}" -lgdi32 -luser32
echo "built build/d3d9.dll (PE32 - this is the file you drop next to the game)"

# test_seam audits the rasteriser with no reference image and carries negative
# controls that must fail; see build.ps1 for the full rationale.
"$ZIG" cc "${WARN[@]}" -Isrc -target x86_64-windows-gnu \
  -o build/test_seam.exe tests/test_seam.c src/swrast.c -luser32 -lgdi32

# The savestate harness drives savestate.c directly, no wrapper and no game.
"$ZIG" cc "${WARN[@]}" -Isrc -target x86_64-windows-gnu \
  -o build/ss_harness.exe tests/ss_harness.c src/savestate.c -luser32

# A tiny D3D9 program that loads the wrapper and draws, so the DLL can be shown
# to work without launching the game. 32-bit, to match it.
"$ZIG" cc "${WARN[@]}" -Isrc -target x86-windows-gnu \
  -o build/d3d9_sw_test.exe tests/test.c -luser32 -lgdi32

echo "built build/test_seam.exe build/ss_harness.exe build/d3d9_sw_test.exe"
echo ""
echo "run ./build/test_seam.exe to verify the rasteriser (under Wine on non-Windows)"
