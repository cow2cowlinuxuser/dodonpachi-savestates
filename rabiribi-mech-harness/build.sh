#!/usr/bin/env bash
# Build the mechanical harness with zig (a clang cross-compiler), matching the
# Rabi-Ribi project's toolchain. The game and its harnesses are 32-bit PE32, so
# that is the build to run; a 64-bit build is produced too, to shake out
# pointer-width assumptions.
set -euo pipefail
cd "$(dirname "$0")"

ZIG="${ZIG:-$(command -v zig || true)}"
if [ -z "$ZIG" ] && [ -x /opt/zig/zig ]; then ZIG=/opt/zig/zig; fi
if [ -z "$ZIG" ]; then echo "zig not found (see repo build.sh / .cursor/setup.sh)" >&2; exit 1; fi

WARN=(-O2 -Wall -Wno-incompatible-function-pointer-types)
mkdir -p build

echo "using $ZIG (zig $("$ZIG" version))"
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/rr_mech_harness32.exe rr_mech_harness.c -luser32
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/rr_mech_harness.exe   rr_mech_harness.c -luser32
echo "built build/rr_mech_harness32.exe (PE32, the one to run) and build/rr_mech_harness.exe (64-bit)"

# Headless D3D11-on-CPU probe (runs under Wine via wined3d + llvmpipe/lavapipe).
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/d3d11_probe.exe   d3d11_probe.c -ld3d11 -ldxgi
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/d3d11_probe32.exe d3d11_probe.c -ld3d11 -ldxgi
echo "built build/d3d11_probe.exe and build/d3d11_probe32.exe"

# D3D11 device-state save/restore harness (observe mutations, validate restore).
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/d3d11_state_harness.exe   d3d11_state_harness.c -ld3d11 -ldxgi
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/d3d11_state_harness32.exe d3d11_state_harness.c -ld3d11 -ldxgi
echo "built build/d3d11_state_harness.exe and build/d3d11_state_harness32.exe"
