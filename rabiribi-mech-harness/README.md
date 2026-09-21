# rr_mech_harness - a mechanical bounds-mapper for the Rabi-Ribi savestate

A self-contained bench rig for the question: *can we create a save point and put
it back in RAM "as is", as many times over as we like?* It answers by mapping
the bounds - provoking every failure mode the
[Rabi-Ribi savestate](https://github.com/cow2cowlinuxuser/rabiribi-savestate)
documentation names, showing the mitigation that steps around each, and then
iterating to confidence.

It does **not** link the real 680 KB `savestate.c` and does **not** need the
game. It models the ownership shapes that matter and runs its own compact
in-memory snapshot/restore over one pinned arena, so it builds and runs anywhere
`zig` + Wine do - including this repo's Cloud Agent environment.

## Build & run

Needs `zig` (installed by this repo's `.cursor/setup.sh`). The game and its
harnesses are 32-bit PE32, so `rr_mech_harness32.exe` is the one to run; a
64-bit build is produced too.

```bash
./build.sh
# under Wine on Linux:
WINEPREFIX=$HOME/.wine-ddp WINEDEBUG=-all wine build/rr_mech_harness32.exe [soak-cycles] [workers]
# e.g.
WINEPREFIX=$HOME/.wine-ddp wine build/rr_mech_harness32.exe 5000 8
```

Exit code is 0 when the soak came back byte-for-byte on every restore.

## What it reports

1. **Negative controls** - three deliberate faults that must each be detected,
   so a silent pass is only trusted after the instrument is shown to bite.
2. **Restore-side boundary map** - for each restore failure mode, the pitfall
   (mitigation OFF, must be DETECTED) beside the mitigation (ON, must stay
   clean): Class A outward straddle, Class B inward straddle, DirectSound cursor
   split.
3. **Capture strategy table** - the heart of it. For RACE / SUSPEND /
   SUSPEND+SETTLE / SUSPEND+VERIFY / BARRIER it reports committed / refused /
   poisoned. *Poisoned* (a committed snapshot that then restores wrong) is the
   number that matters: RACE and SUSPEND poison; **SUSPEND+VERIFY never does**,
   because it verifies the captured bytes and refuses rather than commit a torn
   snapshot. A forced-refusal row proves a refusal is a retry, not a corruption.
4. **Soak** - thousands of cycles with the portable fix (SUSPEND+VERIFY) and the
   Class A/B stressors on, expecting zero poisoned slots; the confidence
   statement.

See [FINDINGS.md](FINDINGS.md) for the measured bounds, the capture fix, and the
concrete change recommended for the real engine.

## Boundaries modeled (all from the source repo's docs)

| Boundary | Doc | Pitfall | Mitigation |
| --- | --- | --- | --- |
| Class C mid-mutation | `restore_invariants.md` | copy while a writer is mid-update | safe-point barrier |
| Capture method | `windows_savestate_ownership.md` + Wine notes | `SuspendThread` freezes a writer mid-update | cooperative barrier (no arbitrary freeze) |
| Class A outward straddle | `restore_invariants.md` | present target moves on under a rewound ref | record-and-replay |
| Class B inward straddle | `restore_invariants.md` | rewind reverts a block a present thread holds | exclude / re-pin |
| Audio cursor split | `ds_harness.c`, `audio_and_archive.md` | rewound write cursor behind the play cursor | reseat on restore |
| Determinism | `determinism.md` | — (always checked) | pinned arena + reproducible RNG/clock |
| Heap metadata | `restore_invariants.md` #2 | — (always checked) | free-list walk, `HeapValidate`-style |
| Thread set | `restore_invariants.md` #4 | — (always checked) | fresh/gone roster diff |

## D3D11 on the CPU (`d3d11_probe.c`) - toward GPU state under restore

The open question is whether GPU-side D3D11 state can die and live by the same
in-process restore mechanism. Step one is a D3D11 device that renders with no
real GPU and no window, so it can eventually be driven under the harness.

`d3d11_probe.c` is that step: a headless D3D11 program (device -> offscreen
render target -> clear -> one shaded triangle with runtime-compiled shaders ->
staging read-back -> dump). Under Wine it routes `HARDWARE` through wined3d to
the software backend (llvmpipe / lavapipe), so the whole D3D11 pipeline runs on
the CPU.

```bash
./build.sh
WINEPREFIX=$HOME/.wine-ddp DISPLAY=:1 wine build/d3d11_probe.exe out.ppm   # or d3d11_probe32.exe
```

Measured here: device created at **feature level 11.0** on the CPU backend, both
PE32 and 64-bit; the triangle renders (corner = magenta clear, interior =
Gouraud blend). That establishes the substrate. The next step is to place the
D3D11 device's own writable state (and the wined3d/driver objects that straddle
the boundary) into the capture/verify/restore loop and find where the GPU side
sits in the same Class A/B/C map - in particular which device state is present
(outside the snapshot) versus rewound, and whether a verify-or-refuse capture
holds when a GPU/driver object is live.

## Scope

The CPU-side harness exercises the *mechanics* of save/restore for these
ownership shapes. It does not yet reproduce the real engine's live-heap rewind,
stack/context rewind, or full GPU/audio device ownership - those are
Windows-internal and are what the real `savestate.c` (and its Wine notes)
wrestle with. The value here is the map: each bound is named, provokable on
command, and shown to have a way around it - and now a CPU-runnable D3D11 device
to extend that map onto the GPU side.
