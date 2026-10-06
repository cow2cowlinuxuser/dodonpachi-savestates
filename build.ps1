$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# zig is used as the C compiler because it cross-compiles to 32-bit Windows out
# of the box with no SDK install, which is the only awkward part of building a
# PE32 DLL on a modern machine. Any clang that can target i386-windows-gnu would
# do; nothing here depends on zig itself.
$zig = $env:ZIG
if (-not $zig) { $zig = (Get-Command zig -ErrorAction SilentlyContinue).Source }
if (-not $zig -and (Test-Path "C:\zig\zig.exe")) { $zig = "C:\zig\zig.exe" }
if (-not $zig) {
  Write-Error "zig not found. Install from https://ziglang.org/download/ and put it on PATH, or set `$env:ZIG."
}

# Printed so that a build log pasted into a bug report says which compiler
# produced it, which is the first thing anyone would otherwise have to ask.
Write-Host "using $zig (zig $(& $zig version))"

$warn = @("-O2", "-Wall", "-Wno-incompatible-function-pointer-types")
$src = @(
  "src/d3d9_sw.c", "src/swrast.c", "src/savestate.c", "src/vsinterp.c",
  "src/trace.c", "src/tramp.c", "src/allocwatch.c", "src/dsoundhook.c",
  "src/ds_sw.c", "src/xa2_sw.c", "src/gameheap.c", "src/phase.c", "src/logdir.c",
  "src/d3d9.def"
)
# The engine files every harness links, minus the renderer.
$engine = @("src/savestate.c", "src/dsoundhook.c", "src/ds_sw.c", "src/xa2_sw.c", "src/gameheap.c", "src/phase.c", "src/logdir.c")

New-Item -ItemType Directory -Force -Path "build" | Out-Null

# The game is a 32-bit process, so this is the one that actually ships.
# A fixed base: a save records addresses inside this DLL, and a later launch
# has to put it back at the same place for a save from that launch to load.
& $zig cc @warn -Isrc -DD3D9SW_VARIANT=stock -target x86-windows-gnu -shared -o build\d3d9.dll @src -lgdi32 -luser32 -lwinmm "-Wl,--image-base=0x60000000" "-Wl,--no-dynamicbase"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built build\d3d9.dll (PE32 - this is the file you drop next to the game)"

# test_seam.exe has no reference image to compare against. It renders additively
# so every pixel counts how many triangles claimed it, and blits from a texture
# whose every texel is distinct, so coverage gaps, doubled pixels and wrong
# texels are each named rather than eyeballed. It carries deliberate negative
# controls that must fail, because a clean run only means something if the
# instrument can still detect a fault.
& $zig cc @warn -Isrc -target x86_64-windows-gnu -o build\test_seam.exe tests\test_seam.c src\swrast.c -luser32 -lgdi32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# The harness drives savestate.c directly, with no wrapper and no game, so an
# invariant can be asserted after every restore instead of waiting to see
# whether something dies later. Built from the same source the game loads: a
# harness against a copy of the engine would prove nothing about the engine.
& $zig cc @warn -Isrc -target x86-windows-gnu -o build\ss_harness.exe tests\ss_harness.c @engine -luser32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# A tiny D3D9 program that loads the wrapper and draws, so the DLL can be shown
# to work without launching the game. 32-bit, to match it.
& $zig cc @warn -Isrc -target x86-windows-gnu -o build\d3d9_sw_test.exe tests\test.c -luser32 -lgdi32 -lwinmm
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "built build\test_seam.exe build\ss_harness.exe build\d3d9_sw_test.exe"
Write-Host ""
Write-Host "run .\build\test_seam.exe to verify the rasteriser"
