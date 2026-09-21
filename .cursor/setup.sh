#!/usr/bin/env bash
# Cloud Agent install step. Idempotent: safe to run repeatedly and against a
# snapshot that already has the toolchain. It ensures the zig cross-compiler and
# Wine are present, then builds the DLL and test binaries with build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG_VERSION="0.15.2"

# --- zig (the C cross-compiler the project builds with) -----------------------
if ! command -v zig >/dev/null 2>&1 && [ ! -x /opt/zig/zig ]; then
  echo "installing zig ${ZIG_VERSION}"
  tmp="$(mktemp -d)"
  curl -fsSL -o "$tmp/zig.tar.xz" \
    "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"
  sudo rm -rf /opt/zig
  sudo mkdir -p /opt/zig
  sudo tar -xf "$tmp/zig.tar.xz" -C /opt/zig --strip-components=1
  sudo ln -sf /opt/zig/zig /usr/local/bin/zig
  rm -rf "$tmp"
fi
zig version

# --- Wine (to run the Windows test binaries on Linux) -------------------------
if ! command -v wine >/dev/null 2>&1; then
  echo "installing wine"
  sudo dpkg --add-architecture i386
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    wine wine32:i386 wine64
fi
wine --version

# A dedicated 32-bit-capable Wine prefix, created once. WINEDEBUG quiets the
# noise; the prefix persists in the snapshot so this is a no-op on later boots.
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-ddp}"
export WINEDEBUG="${WINEDEBUG:--all}"
if [ ! -d "$WINEPREFIX" ]; then
  echo "initialising wine prefix at $WINEPREFIX"
  wineboot -i >/dev/null 2>&1 || true
  wineserver -w || true
fi

# --- build --------------------------------------------------------------------
./build.sh

echo "setup complete: build/ contains d3d9.dll and the test executables"
