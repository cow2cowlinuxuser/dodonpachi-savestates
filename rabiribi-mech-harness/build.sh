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

# D3D11 cross-session: save a snapshot to disk in one process, reproduce it in a
# fresh process (fresh wined3d device).
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/d3d11_xsession.exe   d3d11_xsession.c d3d11_scene.c -ld3d11 -ldxgi
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/d3d11_xsession32.exe d3d11_xsession.c d3d11_scene.c -ld3d11 -ldxgi
echo "built build/d3d11_xsession.exe and build/d3d11_xsession32.exe"

# D3D11 coexistence: run the savestate capture/restore loop WITH a live wined3d
# device in the process (Class B held pointer; --retire for Class A straddle).
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/d3d11_coexist.exe   d3d11_coexist.c -ld3d11 -ldxgi
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/d3d11_coexist32.exe d3d11_coexist.c -ld3d11 -ldxgi
echo "built build/d3d11_coexist.exe and build/d3d11_coexist32.exe"

# GDI/USER coexistence: savestate loop WITH a live window + GDI DIB/DC.
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/gdi_coexist.exe   gdi_coexist.c -lgdi32 -luser32
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/gdi_coexist32.exe gdi_coexist.c -lgdi32 -luser32
echo "built build/gdi_coexist.exe and build/gdi_coexist32.exe"

# COM audio probe (XAudio2 / mmdevapi / DirectSound under Wine).
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/audio_probe.exe   audio_probe.c -lole32 -luuid -ldsound
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/audio_probe32.exe audio_probe.c -lole32 -luuid -ldsound
echo "built build/audio_probe.exe and build/audio_probe32.exe"

"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/audio_coexist.exe   audio_coexist.c -lole32 -luuid
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/audio_coexist32.exe audio_coexist.c -lole32 -luuid
echo "built build/audio_coexist.exe and build/audio_coexist32.exe"

"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/audio_xsession.exe   audio_xsession.c -lole32 -luuid
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/audio_xsession32.exe audio_xsession.c -lole32 -luuid
echo "built build/audio_xsession.exe and build/audio_xsession32.exe"

"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/present_cb_harness.exe   present_cb_harness.c d3d11_scene.c -ld3d11 -ldxgi
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/present_cb_harness32.exe present_cb_harness.c d3d11_scene.c -ld3d11 -ldxgi
echo "built build/present_cb_harness.exe and build/present_cb_harness32.exe"

"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/combined_harness.exe   combined_harness.c d3d11_scene.c -ld3d11 -ldxgi -lole32 -luuid -lgdi32 -luser32
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/combined_harness32.exe combined_harness.c d3d11_scene.c -ld3d11 -ldxgi -lole32 -luuid -lgdi32 -luser32
echo "built build/combined_harness.exe and build/combined_harness32.exe"

"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/dxgi_swapchain_harness.exe   dxgi_swapchain_harness.c d3d11_scene.c -ld3d11 -ldxgi -luser32
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/dxgi_swapchain_harness32.exe dxgi_swapchain_harness.c d3d11_scene.c -ld3d11 -ldxgi -luser32
echo "built build/dxgi_swapchain_harness.exe and build/dxgi_swapchain_harness32.exe"

"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o build/atlas_batch_harness.exe   atlas_batch_harness.c d3d11_scene.c -ld3d11 -ldxgi
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o build/atlas_batch_harness32.exe atlas_batch_harness.c d3d11_scene.c -ld3d11 -ldxgi
echo "built build/atlas_batch_harness.exe and build/atlas_batch_harness32.exe"
