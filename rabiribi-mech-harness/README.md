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
Gouraud blend). That establishes the substrate.

### `d3d11_state_harness.c` - save/restore of D3D11 device state

Builds on the probe to answer the two questions that matter for a savestate,
without pretending to rewind true driver/GPU memory:

- **Does the device keep existing across save/restore?** One persistent
  `ID3D11Device` and its resources are reused for every cycle and polled for
  removal.
- **Can we observe mutations, and validate none after a restore?** Two views of
  state are compared each cycle: the *logical* state (a GPU-resident constant
  buffer, read back FROM the device) and the *visible* state (the rendered
  framebuffer, fingerprinted). "Restore" writes the saved bytes back into the
  GPU resource and re-renders.

```bash
WINEPREFIX=$HOME/.wine-ddp DISPLAY=:1 wine build/d3d11_state_harness.exe 300
```

Measured (300 cycles/mode, CPU backend):

| mode | result |
| --- | --- |
| NOOP | visible_equal 300/300, cb_equal 300/300 (no visible mutation) |
| MUTATE+RESTORE | mutation_observed 300/300, then visible_equal 300/300, cb_equal 300/300 |
| MUTATE-only (control) | mutation_observed 300/300, visible_equal 0/300 |

Device stayed alive the whole run (0 removal events, ~2100 draws). So the device
persists, a mutation is observable in both the logical constant buffer and the
rendered frame, and a content-level restore leaves **no visible mutation** - with
the control confirming the comparison actually bites.

### `d3d11_xsession.c` - close the program, reopen it, reproduce the snapshot

The real prize: can a snapshot taken in one session be reproduced after the
process exits? Each run is a **separate process** with a brand-new
`ID3D11Device` (and, under Wine, a fresh wined3d instance), so a match proves we
coexist with wined3d well enough that the 3D state is reproducible across
sessions.

A snapshot holds the GPU-resident logical state (the constant buffer) plus the
exact framebuffer it produced. `save` renders and writes both to disk and exits;
`restore` starts fresh, loads the snapshot, writes the logical state into a NEW
device, re-renders, and compares hash + pixel-for-pixel.

```bash
wine build/d3d11_xsession.exe save    snap.bin 12345
wine build/d3d11_xsession.exe restore snap.bin out.ppm   # a separate process
```

Measured (each line a distinct PID / fresh device, CPU backend):

| session | result |
| --- | --- |
| A save (seed 12345) | fp=0xffeb184e written to disk |
| B restore (fresh process) | reproduced fp=0xffeb184e, **pixel_diffs 0/65536**, PASS |
| C restore (fresh process) | reproduced fp=0xffeb184e, **pixel_diffs 0/65536**, PASS |
| seed 777, save+restore | reproduced fp=0x81380854, **0/65536**, PASS |
| negative control (1 byte of stored frame flipped) | pixel_diffs 1/65536, FAIL (comparison bites) |

So a fresh process reproduces a previously taken snapshot **bit-for-bit**. This
works because the draw is deterministic (opaque triangle, no blending) and the
logical state fully determines the frame - the two properties a cross-session
savestate needs. It matches the "same-boot cross-session restore" the source
repo's `determinism.md` flagged as testable-but-untested; here it is tested, for
the D3D11 device-state slice, and it holds.

### `d3d11_coexist.c` - the savestate loop WITH a live wined3d device

The gating question: does Wine block us? A real D3D11 device spins up wined3d's
own threads and driver objects. This harness stands one up and runs the arena
capture/verify/restore loop (our worker threads churning a rewound arena,
quiesced at a cooperative safe point) interleaved with rendering, holding the
live device pointer INSIDE the rewound arena.

```bash
wine build/d3d11_coexist.exe 300            # Class B: device held through the rewind
wine build/d3d11_coexist.exe 120 --retire   # Class A: device recreated each cycle
```

Measured (CPU backend, wined3d live throughout):

| mode | result |
| --- | --- |
| Class B (device pointer carried through the rewind), 300 cycles | poisoned 0, render_mismatch 0, Class B pointer valid 300/300, device removals 0, **PASS** |
| Class A (`--retire`, device recreated between save and restore), 120 cycles | straddle detected 120/120, re-pinned 120/120, render_mismatch 0, poisoned 0, **PASS** |

What this establishes:

- **Wine does not block us.** The whole loop runs to completion with wined3d
  live - even recreating the device every cycle 120 times - with no deadlock.
  The reason: we quiesce OUR workers at a cooperative safe point and never
  `SuspendThread` wined3d's threads, side-stepping the suspend-all-then-wait-on-
  wineserver hazard the source repo's notes warn about.
- **Class B holds with a real object.** The live device pointer, carried through
  a memcpy-rewind of the arena, is still valid every time and still renders the
  restored frame.
- **Class A is handled safely.** When the present side "moves on" (device
  released and recreated), the arena's rewound handle carries a stale generation;
  we DETECT that via a generational handle instead of calling through a dangling
  COM pointer, and re-pin to the current device (object retirement). The frame
  still reproduces after re-pin, because the render is deterministic across
  device instances (same property proven in `d3d11_xsession`).

So a real wined3d/D3D11 object can live inside the capture/restore loop, sit in
the Class A/B map exactly where the docs predicted, and be handled - and Wine is
not the blocker. The next step is depth: bring the wined3d object *graph*
(views, buffers, shaders, driver-side allocations) under the same handle/verify
discipline rather than a single device pointer.

### `gdi_coexist.c` - GDI/USER32 as a possible upstream blocker

The docs flag GDI/USER32 ("USER32/GDI not rewound; Proton A->B dies in USER32").
This harness stands up a real window + a GDI memory DC + a DIB section, holds the
`HWND`/`HDC`/`HBITMAP` handles inside the rewound arena, and runs the
capture/restore loop around them.

```bash
wine build/gdi_coexist.exe 200
```

Measured (200 cycles, GDI/USER live throughout):

| check | result |
| --- | --- |
| handles valid after restore | **200/200** - HWND/HDC/HBITMAP survive the rewind |
| GDI content split observed | **200/200** - the DIB pixels are NOT rewound |
| GDI reconciled by redraw | **200/200** - redraw from rewound state fixes it |
| arena poisoned / deadlock | 0 / none (ran to completion) |

Empirical finding, in the same Class A/B/split vocabulary:

- **GDI/USER handles are kernel-side and persist across the rewind.** The handle
  value rewinds with the arena and still addresses the live win32k object, so
  *same-session* a held HWND/HDC/HBITMAP is valid after a restore. Not a blocker
  for handle validity.
- **GDI object *content* is a split, like the audio cursor.** A DIB's pixels
  live on the GDI side, outside our snapshot; a process-memory rewind does not
  touch them. This is a real desync source and must be reconciled - here by
  redrawing the content from the rewound logical state (200/200).
- **A live window + GDI + message pump does not block the cooperative capture.**
  Because we never `SuspendThread` the UI thread, no win32k lock is ever held by
  a frozen thread, so there is no deadlock.

Caveat that matches the docs' "Proton A->B dies in USER32": the *cross-session*
case is different. HWND/HDC/HBITMAP values belong to a process's own USER/GDI
handle namespace, so a handle saved in session A is meaningless in session B -
it must be recreated and re-pinned (the same object-retirement pattern as the
D3D device in `--retire`), not restored as a raw value. Same-session persistence
(shown here) and cross-session recreation are two different problems; this maps
the first and names the second.

## Scope

The CPU-side harness exercises the *mechanics* of save/restore for these
ownership shapes. It does not yet reproduce the real engine's live-heap rewind,
stack/context rewind, or full GPU/audio device ownership - those are
Windows-internal and are what the real `savestate.c` (and its Wine notes)
wrestle with. The value here is the map: each bound is named, provokable on
command, and shown to have a way around it - and now a CPU-runnable D3D11 device
to extend that map onto the GPU side.
