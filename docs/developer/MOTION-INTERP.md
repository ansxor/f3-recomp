# Experimental GPU motion interpolation

## Decision and supported surface

Opt-in **`--motion-interp`**, default off, CLI-only. Implemented on SDL3 GPU
presentation (`--video-backend gpu`, `--video game|compare`); measured on
macOS arm64 / Cocoa / Metal. CPU/FDP presentation is not interpolated.
Headless execution, native dumps, replay data and machine/netplay snapshots
remain canonical. Game-data video still requires strict-native `landmakrj`;
this feature does not permit `--allow-fallback`.

```sh
./build/landmakr --video-backend gpu --video-scale auto-integer --motion-interp
# Independent spatial line sampling can be combined with temporal motion:
./build/landmakr --video-backend gpu --video-scale 3 \
  --video-interp fit --motion-interp
```

This is **geometry interpolation**, not image cross-fading, extrapolation,
optical flow or an increased emulation rate. It adds one native frame of
positional latency: approximately 16.97 ms at `6671500 / (432 * 262)` =
58.9438 Hz, before presentation/queue latency. Current artwork, animation tiles,
palette, clipping, blending and ordering remain discrete. Native scale 1 still
quantizes to native pixels; scale 2–4 exposes fractional movement better.

## Investigation and implementation

- `GameVideo::capture_gpu` exports the rendered, latched semantic scene before
  the next sprite latch. `GpuScene` is already host-only and is not serialized.
  CPU native composition and diagnostic/reference rendering remain separate.
- Existing `--video-interp off|linear|fit` is **spatial** sampling between
  playfield scanlines above native scale. `gpu_interp.cpp` validates controls
  and builds appended coefficients; it does not keep consecutive frame history.
- `gpu_video.cpp` uploads packed scene words, rasterizes ROM sprite texels,
  composes playfields/text and presents through SDL GPU. Its original `draw`
  remains the canonical GPU diagnostic path.
- The CPU backend produces completed pixel images, including lazy expanded
  diagnostic images. Rebuilding every CPU scene at display frequency would
  duplicate that compositor path. This experiment therefore supports GPU only.

New `runtime/gpu_motion.hpp/.cpp` stores a fixed-capacity geometry/control
snapshot and precomputed deltas. The previous coordinate is recoverable as
`current - delta`; full previous images, tilemaps, reference scenes and native
pixels are not copied into temporal history. Eligibility is calculated once
per completed emulated frame. `apply` modifies the already-mapped GPU upload,
not the canonical `GpuScene`; repeated presentations allocate no motion storage.
The off path never allocates temporal history.

GPU API:

- `capture_motion(scene, frame)`: explicit native-frame capture, lazy allocation.
- `reset_motion()`: discard frame pairing; retain reusable allocation.
- `draw_motion(scene, alpha, ...)`: consume history without advancing it.
- `last_motion()`: eligible moving sprite/PF/text counts and actual alpha.

Alpha 0 uses previous eligible positions with **current** discrete content;
alpha 1 is pixel-exact to ordinary GPU drawing for the same spatial settings.
Invalid/nonfinite alpha uses current geometry. Ordinary `draw` never enables
motion. Caller layer-mask high bits are stripped before applying the internal
motion marker.

Playfield X is already 24.8. Y interpolates the combined
`source_y * 256 + y_fraction`, preserving existing X/Y zoom. Text scroll uses
motion-only 24.8 values in spare packed row words 14/15, selected by an internal
uniform marker; canonical integer text fields and word offsets do not change.
Fractional text sampling evaluates output-grid positions; integer endpoints
retain the native replication rule. Spatial interpolation analyzes effective
upload geometry when temporal motion is active, while retaining the original
control/clip and palette safety checks. Alpha 1 uses the original shader path.

## Presentation clock and discontinuities

The frontend gates solo native steps against the existing monotonic native
frame period, but does not sleep until the next native step after each motion
draw. SDL GPU swapchain acquisition paces repeated presentations at the display
cadence. Alpha is elapsed monotonic time within the current native period,
clamped to `[0,1]`. Temporal capture is tied to native advancement or an explicit
history reset, not exposure/resize redraws. Temporal window mode requests one
GPU frame in flight; ordinary rendering keeps its existing queue settings.

The network advance deadline, lead limit, input words, session pump and
rollback execution are unchanged. Additional presentation iterations do not
advance the machine. History resets on:

- local state loads;
- network rollback-count changes and session discontinuities/handoff;
- connect/disconnect and solo pause/resume;
- a solo clock resync after a stall beyond the existing 50 ms catch-up window;
- fallback capture/draw, duplicate/backward or nonconsecutive frame identities.

The first recovered frame snaps. No extrapolation while stalled; alpha clamps
at current geometry. `--unthrottled` uses current geometry (alpha 1), without
synthesizing timed intermediate frames. Finite `--surface` capture also forces
alpha 1. F12 GPU screenshots may contain the visible interpolated geometry;
`--dump-dir` and headless/native captures remain CPU-produced.

## Matching and snap policy

### Sprites

Match the compact submitted **slot index**, only with the same submitted count,
pen mask, tile, palette, X/Y scale and flip bits. Require valid coordinates,
scales and asset indices. Lerp X/Y in 24.8, nearest-rounding the fixed-point unit.

Snap instead of lerp for appearance/disappearance (a count change snaps all
sprites), slot replacement, tile/animation changes, palette changes, zoom/flip
changes, invalid data or movement over **32 native pixels per axis per frame**.
Raw coordinate wrap jumps also exceed the guard; no modular shortest-path lerp.

This is not game-object tracking. Identically textured reordered slots can be
ambiguous, and changing sprite counts deliberately sacrifices smoothing of
otherwise stable sprites. These conservative limitations are intentional.

### Playfields, line scroll and text scroll

Check each visible row 24–255 independently, for each playfield and text layer.
Require enabled, valid, nonmosaic controls, unchanged blend/order/mosaic and
clip ranges/reference bits. Playfield X/Y zoom must be unchanged and valid.
Interpolate source coordinates only; never interpolate priorities, alpha,
palette-bank roles, tile/glyph content, clips or mosaic.

Reject deltas over 32 native pixels per axis. Reject period crossings (1024-pixel
PF X, 512-pixel PF Y/text), rather than choosing a shortest wrapped path.
Bitmap/invalid/disabled/control-changing rows snap independently. This also
covers per-row line/column-derived source offsets without inventing zoom.

## Build provenance and commands

Worktree `wt/motion-interp`, branch `motion-interp`, based on integration
`6aae6f6`. Release build with AppleClang 21.0.0.21000101. Full-coverage native
profile tiers; no slim mode and no CPU fallback opt-in. ROM directory:
`/Users/darien/Workspace/f3-stuff/roms/landmakr`, validated as Japanese ROMs
(the directory name is not a revision). Generated sound ROM CRC: `5a7e9117`.
Generated main/sound code and all captures stay under ignored `build/`.

Run from the new worktree:

```sh
PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages \
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DF3_ROM_DIR=/Users/darien/Workspace/f3-stuff/roms/landmakr
cmake --build build --target landmakr f3rt-motion-regression \
  f3rt-gpu-regression f3rt-netplay-oracle f3rt-check f3rt-state-check \
  f3rt-frontend-check f3rt-hle-check -j 8
ctest --test-dir build --output-on-failure
```

Build succeeds; CTest **4/4** passes (devices, HLE audio, snapshot validation,
frontend input). Linker emits duplicate-library and Musashi common-section
alignment warnings; no new C++ compile error/warning was observed.

## ROM invariance and intermediate-render proof

`tools/motion_interp_regression.cpp` cold-boots two independently seeded
strict-native machines. One is repeatedly presented; the other is not.
Native pixels/audio are compared every frame. At sampled frames it compares
serialized bytes/sync CRCs, current GPU endpoints, nonfinite-alpha snapping and
repeated frozen draws. It also checks reset/gap/duplicate/backward/fallback
recovery and actual final-frame state-load/reexecution, including native audio.
Guard fixtures cover sprite identity/count/scale/flip/pen mask, the 32-pixel
boundary, PF fixed-point fractions/control changes/bitmap/wraps and text scroll.

```sh
build/f3rt-motion-regression --frames 4000 --seed 12345 --scale 3 \
  --every 20 --interp off --dump-dir build/motion-evidence/off
build/f3rt-motion-regression --frames 4000 --seed 12345 --scale 3 \
  --every 20 --interp linear --dump-dir build/motion-evidence/linear
build/f3rt-motion-regression --frames 4000 --seed 12345 --scale 3 \
  --every 20 --interp fit --dump-dir build/motion-evidence/fit
```

All three spatial modes pass, with identical native outcomes:

| Per mode | Observed result |
| --- | --- |
| Native frames / samples | 4000 / 201 |
| Supported / fallback frames | 3769 / 231 |
| Sampled paired frames | 190 |
| Visible intermediate / midpoint frames | 63 / 63 |
| Eligible moving geometry across phase draws | 75,267 |
| Native pixel CRC | `1782af13` |
| Audio CRC / signed samples | `12d140ec` / 4,039,234 |
| Serialized state / sync CRC | `dbdd0aab` / `637802aa` |
| CPU fallback instructions | 0 |

Alpha 1, repeated draws and state/native/audio parity are exact. The harness
also exercises fixed half/twelfth phase grids; these approximate high-refresh
presentation phases, **not a physical 144 Hz monitor measurement**.

App-owned captures at frame 1201 have distinct intermediate CRCs:
alpha 0 `8750c695`, alpha 0.5 `69063c18`, alpha 1 `e6b48dc4` (3x).
Inspected first-moving captures and final native/GPU selection-scene captures;
intermediate position differences are visible without image blending.

Additional seed-5 4x fit run:

```sh
build/f3rt-motion-regression --frames 1560 --seed 5 --scale 4 \
  --every 20 --interp fit --dump-dir build/motion-evidence/gameplay
```

79 samples, 68 paired, 17 visibly different midpoint frames; 1,206 eligible
sprite geometry draws and 36,360 eligible PF-row draws. No moving text rows in
this sampled ROM sequence; text-scroll geometry is separately fixture-covered.
Native/audio/state/sync CRCs: `a38b55e4` / `4c7823c0` / `85e52564` / `3d402d65`.
The inspected `final_1560_phase_{0,1,2}.png` and `final_1560_native.png` show
player selection, moving name/character sprites and a spatially sampled floor.

The same 4x run also renders a captured game glyph through an isolated text
scene, with separate horizontal and vertical half-native-pixel scroll.
`TEXT_SHADER horizontal_vertical_half_pixel=exact scale=4` passes: the midpoint
is visibly different from both endpoints, and every interior pixel matches
the independent baseline translated by two output pixels. This exercises the
fractional text shader path; it is an induced fixture, not observed ROM text
animation.

## Actual high-refresh frontend and canonical captures

```sh
build/landmakr --video game --video-backend gpu --video-scale 3 \
  --motion-interp --frames 1800 --no-audio --audio-backend accurate \
  --sound-driver native --config build/motion-evidence/clean.cfg \
  --surface build/motion-evidence/clean-final.png \
  --wav build/motion-evidence/clean.wav
```

Actual Cocoa/Metal window reports **120 Hz** display, 58.9438 Hz emulation,
960×696 internal pixels. Observed 1800 native frames, **3664 presentations**,
433 eligible intermediate presentations, 4 history resets, alpha range
0.135777–1; elapsed 32.882 s. This is about 2.036 presentations/native frame.
Whole-run average is about 111.4 presentations/s, **not an uninterrupted 120 fps
claim**; startup/stalls and resets are included. The earlier concurrent-load
run produced 3630 presentations in 34.6823 s with 8 resets. The app-owned final
attract surface was inspected. No desktop screenshots are retained/committed.

Headless motion-on/off runs at 1800 frames and both live runs have identical
native result: frame CRC `f08f089c`, cycles 488,600,676, native blocks 27,238,881,
zero CPU fallback, audio frames 908,827, peak 1234 and nonzero samples 735,105.
`cmp` passes for on/off native `rendered.argb`, on/off WAV and live/headless WAV.
SHA-256 of `clean.wav`, `headless-off.wav` and `headless-on.wav`:

```text
9053a75ab08a394aa4a8c021e94d82d61090f5e76708803295f40c755092d2cc
```

Headless commands use `--video game --video-backend gpu --headless --frames
1800 --audio-backend accurate --sound-driver native`, with/without
`--motion-interp`, distinct WAV/dump destinations and `--dump-start 1800`.
No window/GPU temporal history is created in either run.

## Existing backend and netplay checks

```sh
build/f3rt-gpu-regression --frames 1800 --seed 5 --scale 3 --border 48 \
  --every 60 --layers --interp off --inject-frame 1407 \
  --inject-bitmap --inject-trails --inject-globalflip --inject-unknown \
  --inject-sprite-boundaries

go build -C netplay/server -o ../../build/netplay-server .
python3 tools/run_netplay_oracle.py --suite impaired --seeds 5 --frames 4000 \
  --sound-driver native --rom-dir /Users/darien/Workspace/f3-stuff/roms/landmakr \
  --log-dir build/motion-evidence/netplay
```

GPU baseline: 48 sampled composites, all nine isolated layer contributions,
**zero mismatching pixels**. Induced bitmap/trails/global-flip/unknown producer
fallback and recovery pass, as do crushed-overlap, mirrored zoom and nominal
sprite cull branches. These are induced boundary checks, not played endings.

Real impaired relay: 80 ms RTT, 20 ms jitter, 3% loss/reorder, including snapshot
chunks. 4000 match frames, one host handoff/local return, native fallback 0;
240/244 rollbacks, maximum depth 16/16. Both peers/reference agree on state
`dd1567b6`, audio `b4570dc3`, 2,019,617 audio samples. No natural match ending in
this finite run. This existing oracle is headless, not a two-window motion-on
netplay session; temporal draw invariance/reexecution is established separately
by the two-machine renderer proof. Wire/snapshot schemas are unchanged. Build
content identity changes with source edits as usual; peers still need the same
build, but may independently choose the motion flag.

A separate `build/cpu-only` build with `F3RT_GPU=OFF`, reusing generated CPU
sources, succeeds. Its 600-frame CPU/headless native smoke has frame CRC
`e384aa4b`, 10,999,398 native blocks, zero CPU fallback. GPU-disabled headless
motion flags also execute canonically. CPU plus `--motion-interp` is rejected
with `--motion-interp requires --video-backend gpu`; game video plus
`--allow-fallback` remains rejected with `Game-data video requires strict
native landmakrj`.

## Qualified limits

- Metal runtime verified; generated SPIR-V compiled, but Vulkan runtime untested.
- Physical 120 Hz mode exercised; no physical 144 Hz+ monitor claim.
- No interpolation of zoom, flips, artwork, opacity, palette changes, priority,
  clipping or mosaic. Safety declines may remain visibly native-rate.
- Slot matching is conservative, not semantic game-object identity.
- Text-scroll ROM movement was not observed in the sampled sequence.
- No exhaustive campaign/ending or physical-board video accuracy claim.
- One native frame of positional latency is an intentional tradeoff, not an
  end-to-end input-latency measurement.
- All image/audio/ROM-derived evidence stays in ignored `build/motion-evidence/`;
  only implementation, meaningful regression source and textual evidence/docs
  belong in the commit.
