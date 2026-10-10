# Experimental GPU motion interpolation

## Decision and supported surface

Opt-in **`--motion-interp`**, default off, also a saved restart preference in F1 → Video. Implemented on SDL3 GPU
presentation (`--renderer enhanced`; developer `compare-gpu`); measured on
macOS arm64 / Cocoa / Metal. CPU/FDP presentation is not interpolated.
Headless execution, native dumps, replay data and machine/netplay snapshots
remain canonical. Game-data video still requires strict-native `landmakrj`;
this feature does not permit `--allow-fallback`.

```sh
./build/landmakr --renderer enhanced --video-scale auto-integer --motion-interp
# Independent spatial line sampling can be combined with temporal motion:
./build/landmakr --renderer enhanced --video-scale 3 \
  --video-interp fit --motion-interp
```

This is **geometry interpolation**, not image cross-fading, extrapolation,
optical flow or an increased emulation rate. It adds one native frame of
positional latency: approximately 16.97 ms at `6671500 / (432 * 262)` =
58.9438 Hz, before presentation/queue latency. Current artwork, animation tiles,
palette, clipping, blending and ordering remain discrete. Native scale 1 still
quantizes to native pixels; scale 2–4 exposes fractional movement better.

For a visible, repeatable A/B comparison, run from this worktree:

```sh
./build/f3rt-motion-regression --demo --frames 1560 --seed 5 --scale 3 \
  --demo-seconds 30
```

One window: **left = native steps, right = interpolated positions**. Watch the
purple floor and portrait/playfield edges pan horizontally. The tool cold-boots
the real ROM, freezes the selection scene, then applies a bounded 2-pixel/native
frame playfield pan to a host-only scene copy. This is explicitly diagnostic
motion, not a claim that gameplay is changed or accelerated. Sprites/artwork and
the machine remain frozen; the pan reverses smoothly at +/-64 pixels.
Escape closes it. The two panes share one display tick and drawable submission.

## Investigation and implementation

- `GameVideo` overwrites its single typed `CapturedFrame`
  (`runtime/renderer/game/captured_frame.hpp`) with the rendered, latched
  semantic scene before the next sprite latch (`GameVideo::captured_frame()`).
  It is host-only and is not serialized. The CPU compositor reads it directly;
  `GpuVideo` encodes it into GPU words per present (`runtime/renderer/gpu/encode.cpp`,
  the only writer of scene words; offsets live in `runtime/renderer/gpu/scene_layout.h`).
- Existing `--video-interp off|linear|fit` is **spatial** sampling between
  playfield scanlines above native scale. `interp.cpp` validates controls
  and builds appended coefficients; it does not keep consecutive frame history.
- `video.cpp` encodes the captured frame into the mapped upload buffer, rasterizes ROM sprite texels,
  composes playfields/text and presents through SDL GPU. Its original `draw`
  remains the canonical GPU diagnostic path.
- The CPU backend produces completed pixel images, including lazy expanded
  diagnostic images. Rebuilding every CPU scene at display frequency would
  duplicate that compositor path. This experiment therefore supports GPU only.

New `runtime/renderer/gpu/motion.hpp/.cpp` stores a fixed-capacity geometry/control
snapshot (typed `SceneRow`/`SceneSprite` copies) and precomputed deltas. The
previous coordinate is recoverable as `current - delta`; full previous images,
tilemaps, reference scenes and native pixels are not copied into temporal
history. Eligibility is calculated once per completed emulated frame by
`capture(const CapturedFrame &, frame)`. Per present, `apply(frame, alpha)`
returns a typed `MotionResult`: the stats plus a `MotionState` holding the
motion-effective per-sprite x/y, per-row per-playfield source X and Y phase, and
per-row text X/Y in 24.8 (text scroll lives here because `SceneRow` only has
integer text scroll). The captured frame is never modified; the order per present
is `apply` -> `analyze_gpu_interpolation(..., const MotionState *, ...)` ->
`encode(frame, options, motion, coefficients, mapped_words)`. Repeated
presentations allocate no motion storage.
The off path never allocates temporal history.

GPU API:

- `capture_motion(scene, frame)`: explicit native-frame capture, lazy allocation.
- `reset_motion()`: discard frame pairing; retain reusable allocation.
- `draw_motion(scene, alpha, ...)`: consume history without advancing it.
- `last_motion()`: accepted geometry, moving candidates, rejection census and alpha.
- `pace_motion()`: pace before selecting the native pair; the next timed draw
  consumes that wait rather than waiting a second display tick. Returns false
  when no native pacer is active; the caller then schedules display deadlines.
- `present_motion(scene, frame_start, period, wait=false)`: acquire before
  sampling live alpha; skip busy GPU frames before uploads/rendering. Returns
  whether a drawable was submitted. Explicit captures can request a wait;
  explicit-alpha `draw_motion` stays deterministic.
- `last_presented()`: accepted non-null drawable submission, **not scanout**.
- `display_hz()`, `requested_display_hz()`, `display_callback_hz()`: selected
  mode, best-effort native display-link request, and observed callback cadence.
- `draw_motion_comparison_timed(...)`: cached native/current image at left,
  temporal image at right. Optional PNG captures the app-owned full-window
  composition submitted to the drawable, not the desktop or physical scanout.

Alpha 0 uses previous eligible positions with **current** discrete content;
alpha 1 is pixel-exact to ordinary GPU drawing for the same spatial settings.
Invalid/nonfinite alpha uses current geometry. Ordinary `draw` never enables
motion. Caller layer-mask high bits are stripped before applying the internal
motion marker.

Playfield X is already 24.8. Y interpolates the combined
`source_y * 256 + y_fraction`, preserving existing X/Y zoom. Text scroll uses
motion-only 24.8 values (`MotionState::Row::text_x/text_y`, encoded into spare
row words `F3_ROW_MOTION_TEXT_X/Y`), selected by an internal
uniform marker; canonical integer text fields and word offsets do not change.
Fractional text sampling evaluates output-grid positions; integer endpoints
retain the native replication rule. Spatial interpolation analyzes the
`MotionState` geometry when temporal motion is active, while retaining the original
control/clip and palette safety checks. Alpha 1 uses the original shader path.

## Presentation clock and discontinuities

Flicker shadows ([SPRITE-UNITS.md](SPRITE-UNITS.md#flicker-shadows)) do not use display pacing: the
GPU presenter draws them as a steady linear-light blend of the scene with and without the shadow.

The wait on the display link is bounded to 50 ms; a timeout marks the link dead and presentation
falls back to the native-frame timer (emulation is never throttled below 58.94 Hz by a hidden or
occluded window; this also applies to `--motion-interp`), polling for ticks to resume display pacing.

The frontend gates solo native steps against the existing monotonic native
frame period. On macOS 14+, opt-in motion creates an `NSWindow` `CADisplayLink`,
requests the screen's maximum refresh rate, and waits its tick **before**
testing the native-frame deadline. Waiting after that test could select an old
pair, cross the deadline, and draw alpha 1 instead of a useful intermediate.
The timed draw then acquires the SDL swapchain **before** sampling alpha.
Capture remains tied to native advancement or explicit resets, not redraws.
Without an active native display pacer, the frontend and frozen comparison demo
use a rolling display-mode deadline. The frontend wakes at the earlier display
or native-simulation deadline, so a slow display does not throttle PCM production.
Paced live draws prefer mailbox presentation and skip busy in-flight GPU frames before
uploads; explicit captures still wait and render the requested frame. See the
[Linux display-backpressure reproduction](GPU-VIDEO.md#linux-presentation-backpressure-and-audio)
for hardware-qualified evidence and FIFO/driver-stall limits. The macOS
hidden/off-display callback wait retains its monotonic 50ms bound.
Temporal window mode requests one GPU frame in flight. Ordinary/default-off
mode does not create a display link and retains SDL's default allowed frames in
flight; both modes share the mailbox presentation preference.

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

Every two seconds, `MOTION` reports selected/requested/callback display Hz,
`drawable_submissions_s`, `native_s`, `interpolated_pct`,
`moving_interpolated_pct`, moving native pairs and explicit history resets.
`MOTION-CANDIDATES` gives sprite/PF/text candidates, accepted geometry and
identity/count-subset/transform/jump/control rejection counts.

- Submission rate counts only successful commands with an acquired drawable;
  it is not a count of draw calls or GPU fence completions.
- `interpolated_pct` divides geometry-interpolated in-between submissions by
  all drawable submissions. Boot, static frames, pauses and alpha-1 endpoints
  legitimately lower it.
- `moving_interpolated_pct` divides accepted moving in-between submissions by
  known moving in-between submissions. Unmatched identities are reported
  separately: their movement cannot safely be inferred.
- Candidate/rejection census is accumulated once per sampled native pair, not
  once per repeated draw. Static unchanged geometry is not a rejection.
  A row accepting one axis and rejecting the other appears in both counts.
- Mode Hz and display-link callbacks are not proof of drawable scanout.
  SDL does not expose Metal drawable-presented callbacks; `scanout_hz=unknown`
  is intentional. A requested 120Hz with callbacks near 60Hz is a pacing/system
  policy issue, not evidence that a geometry candidate was rejected.

## Matching and snap policy

### Sprites

Compact submitted slots are not object IDs: tile-zero entries are omitted, so
insertions/removals can shift otherwise stable sprites. Sprites carrying a sprite-unit
identity match exactly by it when the tile/palette is unchanged. An identity match whose
tile/palette changed is demoted: both sprites join the appearance pass below, and the
identity pairing is restored only if both stay unmatched after it (so a static twin of the
same tile wins; Command War's zoomed fighter grid re-indexes entries by one cell between
frames and otherwise shifted diagonally). An identity that occurs on both sides but is duplicated is
ambiguous and stays unmatched. An identity absent from the other frame entirely (a
birth in the current frame, a vanished sprite in the previous one) falls back to the
appearance pass, since some games re-key a sprite's identity (Command War: the emit
unit's object RAM address changes every two frames while the sprite stays on screen).
Identity-less sprites and these births/vanished sprites match bounded tile/palette
appearance groups, with mutually unique closest positions (32 px window) for duplicate
groups; the two kinds never pair with each other (`key()` bit 63 marks identity-bearing).
Equal-distance ties or non-mutual assignments snap, rather than
arbitrarily selecting a neighbor. Unchanged sprite lists skip matching.

An identity is (invocation, entry index), so a pose change can reassign entry k to a
different body part of a multi-part object, and an appearance-fallback pairing can pair
the wrong parts of one object. Tile/palette-changed identity matches and appearance-fallback
matches of identity sprites therefore interpolate only if they pass a per-object rigid
check: every sprite carries `object` (its invocation identity; splice replacement entries use
`splice.identity`; 0 = unknown, never serialized, excluded from equality). The object's
reference delta is the unique mode of (dx, dy) over its same-tile identity-pass matches;
if it has none, the mode over all its matched entries (identity and fallback) when a strict
majority holds it (a single-entry object trivially passes). A checked entry
interpolates only when its delta is within ±1 native px per axis of that reference. No
reference (tie, no majority), `object == 0`, or a delta disagreeing with the reference snaps;
rejections count as `sprite_transform_rejections`. Same-tile identity-pass matches
(measured clean) and identity-less sprites are unaffected.

Count changes no longer veto surviving matched sprites. Require the same pen
mask, X/Y scales and flip bits, valid coordinates and asset indices, and at
most **32 native pixels per axis per frame**. Lerp X/Y in 24.8, nearest-rounding
the fixed-point unit. Unmatched births/replacements, tile-animation changes that
disagree with their object's rigid motion, palette-only changes under the same test,
zoom/flip changes, invalid data and larger movement snap. Raw coordinate
wrap jumps exceed the guard; no modular shortest-path lerp.

This remains render-snapshot matching, not semantic game-object tracking.
Tile-changing animation has no reliable object ID in this snapshot and remains
discrete. Nearby indistinguishable entities can still be ambiguous; counters
do not relabel them as known valid motion merely to improve a percentage.

### Playfields, line scroll and text scroll

Check each visible row 24–255 independently, for each playfield and text layer.
Require that layer's enabled, valid, nonmosaic state and unchanged clip ranges/
reference bits. Unrelated layer state and global blend/order changes do not
veto its source motion; those compositing fields remain current and discrete.

X/Y eligibility is independent. Preserve the corresponding unchanged valid
zoom, reject deltas over 32 native pixels and raw period crossings (1024-pixel
PF X, 512-pixel PF Y/text). A Y jump/zoom can snap Y while X still interpolates.
Bitmap mode rejects text, not unrelated valid playfield source coordinates.
Never interpolate priorities, alpha, palette-bank roles, tiles/glyphs, clips
or mosaic. Per-row line/column offsets use the same source-coordinate guards.

## Build provenance and commands

Worktree `wt/motion-interp`, branch `motion-interp`, based on integration
`6aae6f6`. Release build with AppleClang 21.0.0.21000101. Full-coverage native
profile tiers; no slim mode and no CPU fallback opt-in. ROM directory:
`/path/to/roms/landmakr`, validated as Japanese ROMs
(the directory name is not a revision). Generated sound ROM CRC: `5a7e9117`.
Generated main/sound code and all captures stay under ignored `build/`.

Run from the new worktree:

```sh
PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages \
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DF3_ROM_DIR=/path/to/roms/landmakr
cmake --build build --target landmakr f3rt-motion-regression f3rt-motion-check \
  f3rt-gpu-regression f3rt-netplay-oracle f3rt-check f3rt-state-check \
  f3rt-frontend-check f3rt-hle-check -j 8
ctest --test-dir build --output-on-failure
```

The initial `c45da19b` build passed four CTests. The follow-up adds the
consumer-behavior `motion-interpolation-guards` test. Original measurements
below are historical; follow-up measurements are recorded separately.
Linker duplicate-library/Musashi alignment warnings predate this feature.

## Initial ROM invariance proof (`c45da19b`)

`tools/motion_interp_regression.cpp` cold-boots two independently seeded
strict-native machines. One is repeatedly presented; the other is not.
Native pixels/audio are compared every frame. At sampled frames it compares
serialized bytes/sync CRCs, current GPU endpoints, nonfinite-alpha snapping and
repeated frozen draws. It also checks reset/gap/duplicate/backward/fallback
recovery and actual final-frame state-load/reexecution, including native audio.
Focused guard fixtures now live in `runtime/motion_interp_check.cpp`, including
count shifts, duplicate identity ties, dense unambiguous duplicate movement,
fixed-point boundaries/fractions, controls, wraps and resets.

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

## Initial live-window attempt counts (`c45da19b`)

```sh
build/landmakr --renderer enhanced --video-scale 3 \
  --motion-interp --frames 1800 --no-audio --audio-backend accurate \
  --sound-driver native --config build/motion-evidence/clean.cfg \
  --surface build/motion-evidence/clean-final.png \
  --wav build/motion-evidence/clean.wav
```

The initial Cocoa/Metal window reported a **120Hz display mode**, 58.9438Hz
emulation and 960x696 internal pixels. It counted 1,800 native frames,
3,664 **draw calls**, 433 eligible intermediate draws and four explicit
history resets; alpha 0.135777–1, elapsed 32.882s. Its reported
“presentations” were draw attempts, not confirmed drawable submissions.
Neither callback cadence nor physical drawable scanout was measured.
The whole-run 111.4 draw calls/s therefore cannot prove or disprove a 60Hz
compositor cap. The concurrent-load run counted 3,630 calls in 34.6823s and
eight resets. App-owned final images were inspected; no desktop captures remain.

Headless motion-on/off runs at 1800 frames and both live runs have identical
native result: frame CRC `f08f089c`, cycles 488,600,676, native blocks 27,238,881,
zero CPU fallback, audio frames 908,827, peak 1234 and nonzero samples 735,105.
`cmp` passes for on/off native `rendered.argb`, on/off WAV and live/headless WAV.
SHA-256 of `clean.wav`, `headless-off.wav` and `headless-on.wav`:

```text
9053a75ab08a394aa4a8c021e94d82d61090f5e76708803295f40c755092d2cc
```

Headless commands use `--renderer enhanced --headless --frames
1800 --audio-backend accurate --sound-driver native`, with/without
`--motion-interp`, distinct WAV/dump destinations and `--dump-start 1800`.
No window/GPU temporal history is created in either run.

## Existing backend and netplay checks

```sh
build/f3rt-gpu-regression --frames 1800 --seed 5 --scale 3 --border 48 \
  --every 60 --layers --interp off --inject-frame 1407 \
  --inject-bitmap --inject-trails --inject-globalflip \
  --inject-sprite-boundaries

go build -C netplay/server -o ../../build/netplay-server .
python3 tools/run_netplay_oracle.py --suite impaired --seeds 5 --frames 4000 \
  --sound-driver native --rom-dir /path/to/roms/landmakr \
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
with `--motion-interp requires --renderer enhanced or compare-gpu`; game video plus
`--allow-fallback` remains rejected with `Game-data video requires strict
native landmakrj`.

## Qualified limits

- Metal runtime verified; generated SPIR-V compiled, but Vulkan runtime untested.
- Built-in 120Hz ProMotion callbacks observed; drawable scanout is unavailable
  through SDL's supported public API. No physical 144Hz+ monitor claim.
- No interpolation of zoom, flips, artwork, opacity, palette changes, priority,
  clipping or mosaic. Safety declines may remain visibly native-rate.
- Appearance/position matching is conservative, not semantic game-object identity.
- Text-scroll movement now occurs in the follow-up ROM census; the separate
  glyph fixture still proves exact fractional shader translation.
- No exhaustive campaign/ending or physical-board video accuracy claim.
- One native frame of positional latency is an intentional tradeoff, not an
  end-to-end input-latency measurement.
- All image/audio/ROM-derived evidence stays in ignored `build/motion-evidence/`;
  only implementation, meaningful regression source and textual evidence/docs
  belong in the commit.

## ProMotion visibility follow-up

User observation: no perceived difference from off on the built-in 120Hz
ProMotion panel. The earlier low intermediate count was not a useful acceptance
metric: it mixed boot/static/endpoints with rejected moving channels.

### Reproduced causes and cutover

- Original sprite capture rejected every sprite when the global count changed.
  Compact slots also shift when tile-zero entries disappear. A probe against
  `c45da19b` rendered a surviving sprite at X=24 instead of its expected
  midpoint X=22 after an unrelated insertion.
- Original row capture compared global blend/order controls, and rejected both
  axes when either zoom/jump guard failed. The baseline probe rendered PF
  X=104 instead of midpoint X=102 after only an unrelated blend change.
  Both probes failed before; retained guard regressions pass after the fix.
- Merely replacing the count veto with “only one neighbor within 32 pixels”
  still rejected dense identical sprite groups. Matching now requires a
  mutually unique **nearest** position, preserving normal small movement while
  still snapping equal-distance identity ties.
- Alpha was sampled before swapchain acquisition, and the native pair was
  selected before display pacing. Crossing a native deadline while waiting
  could present the old pair at alpha 1. Native display-link pacing now precedes
  the native deadline gate; swapchain acquisition precedes alpha sampling.
- Four explicit resets across the original 1,800-frame clean run cannot explain
  most missing motion. Boot/oracle intervals, genuinely static geometry, and
  broad eligibility vetoes must be separated. The new readout exposes them.

### Current ROM and guard evidence

```sh
ctest --test-dir build --output-on-failure
build/f3rt-motion-regression --frames 4000 --seed 12345 --scale 3 --every 20 \
  --interp off --dump-dir build/motion-evidence/followup-off
build/f3rt-motion-regression --frames 1560 --seed 5 --scale 4 --every 20 \
  --interp fit --dump-dir build/motion-evidence/followup-fit
```

Release GPU and GPU-disabled native builds succeed. CTest **5/5** passes.
The new guard test covers insertion/removal, dense duplicate motion, identity
ties, transform/pen changes, exact 32px/+1-subunit limits, PF/text fractions,
own-layer clips, independent-axis snaps, wraps, invalid alpha and resets.

| Final proof | 4,000 frames, seed 12345, 3x/off | 1,560 frames, seed 5, 4x/fit |
| --- | ---: | ---: |
| Samples / paired | 201 / 190 | 79 / 68 |
| Known moving samples | 127 | 18 |
| Accepted / visibly different midpoints | 127 / 127 | 18 / 18 |
| Initial visibly different midpoints | 63 | 17 |
| Sprite/PF/text geometry across nine phase draws | 16,902 / 75,303 / 2,088 | 1,206 / 42,624 / 2,088 |
| Sampled moving sprite/PF/text channels | 2,349 / 8,721 / 232 | 396 / 4,804 / 232 |
| Unknown sprite identities / count-changing subset | 2,492 / 2,028 | 45 / 34 |
| Sprite transform / jump rejections | 428 / 43 | 262 / 0 |
| Row control / transform / jump rejections | 0 / 0 / 1,282 | 0 / 0 / 996 |

These are sampled **native-pair** counts, not physical presentation counts.
Rows accepting one axis can also report a rejected other axis. Unknown identity
counts include births/replacements, non-rigid tile-animation changes and ambiguity;
they are not silently included as known valid movement. In the large proof,
63 of 190 paired samples had no known moving geometry. Every one of the 127
known-moving samples produced a visibly different midpoint, versus 63 visible
midpoints before the fixes. Moving text is now observed in the ROM census
(232 layer/scanline pairs), in addition to the isolated exact glyph fixture.

Both proofs retain exact native pixels, serialized/sync state, native audio,
alpha-1 endpoints, repeated draws and final-frame load/reexecution, with zero
main-CPU fallback. Large-run native/audio/state/sync CRCs remain
`1782af13` / `12d140ec` / `dbdd0aab` / `637802aa`; 4,039,234 signed audio samples.
Small-run CRCs remain `a38b55e4` / `4c7823c0` / `85e52564` / `3d402d65`;
1,575,300 signed audio samples. The 4x fractional text shader check also passes.

### Actual one-window comparison

```sh
build/f3rt-motion-regression --demo --frames 1560 --seed 5 --scale 3 \
  --demo-seconds 12 --dump-dir build/motion-evidence/followup-demo
```

Observed Metal run: requested/mode/callback rates **120/120/120Hz**.
After the first capture/startup interval (116.5 submissions/s), four full
two-second intervals each submitted **120.0 drawables/s**. In-between geometry
was applied on **100% of known moving in-between submissions**; overall
per-interval interpolation was 91.7–93.4%.

Totals: 707 render-only native steps, **1,433 drawable submissions / 1,326
interpolated submissions (92.53%)** over 12 seconds. The frozen emulated frame
remained 1560; serialized/native/sync state stayed exact, zero CPU fallback.
Offscreen canonical/midpoint CRCs are `090f3c24` / `cf8c23ed`.
The actual app-owned 2560x928 split surface `demo-window.png` was inspected:
both panes contain the same real selection scene and distinct pan positions.
This is not a desktop screenshot or a physical scanout measurement.

A second six-second comparison exercised **4x + fit interpolation**: 353
render-only steps, 715 drawable submissions, 660 interpolated submissions,
and 100% interpolation of known moving in-betweens. Frozen state remained
exact, with zero CPU fallback. Its app-owned `demo-window.png` was also
inspected. This run overlapped the canonical GPU regression, so it is not
an isolated performance benchmark.

### Actual frontend readout and default-path checks

A separate 900-frame no-input `landmakr` run used GPU compare mode,
auto-integer scale (4x internal), and `--motion-interp`. Mode/request/callback
rates were 120/120/120Hz; periodic drawable submissions were 118.8–120.0/s,
native frames 58.5–59.3/s, and history resets **zero**.

This run mostly displayed the static notice screen: static intervals
correctly reported zero known moving candidates, zero rejections, and 0%
interpolation. Two transition pairs had known moving geometry and 100%
moving-in-between interpolation. Totals were 1,826 drawable submissions
and just two interpolated submissions. This is evidence that static
frames are not eligibility failures, not a steady-motion benchmark.

Canonical compare mode checked 669 supported frames / 49,666,560 pixels
with **zero mismatches**; the other 231 frames used the existing startup
fallback. Final native rendered pixels, main RAM, graphics, palette,
control, shared RAM, and CPU dump were byte-identical to a separate
900-frame headless native run. Both WAVs were byte-identical with SHA-256
`c75295d38aa2842709951d78d39ead2025c1bd6aa46eb371f18a51605b089869`.
That attract-screen audio was silent; the seeded ROM proofs above supply
the nonzero-audio parity evidence.

The CPU-only build also completed 900 native frames with
`frame_crc=0x9fe1b4e9`, 15,026,654 native blocks, and zero CPU fallback.
CTest passed all five checks, including the standalone motion guards.
The existing 1,800-frame GPU regression passed at 3x + fit with layer
checks and bitmap/trails/global-flip/unknown-control/sprite-boundary
injections: 1,569 supported frames, 231 expected fallback frames, zero CPU
fallback, `frame_crc=0xfb9bec22`, and nonzero native audio. No canonical
rendering mismatch was observed.

### Pacing evidence and limits

Installed SDL3: **3.4.16**. Its Metal backend waits in-flight GPU fences and
uses `CAMetalLayer nextDrawable`/`presentDrawable`; a fence is not a display
presentation callback. The public SDL GPU API exposes no Metal drawable
presented-handler. We therefore report measured display-link callbacks and
accepted drawable submissions separately, and leave scanout unknown.

The supported macOS 14+ `NSWindow` display link requests
`NSScreen.maximumFramesPerSecond` through `preferredFrameRateRange`. This is a
best-effort request; display mode, power/thermal policy or system load can
reduce callback cadence. The prior 60Hz-cap hypothesis is **unproven** because
the initial run did not observe callbacks/scanout. The corrected observed
120Hz callback/submission rates show no 60Hz cap in these exercised runs.
These measurements are macOS-specific; Linux Vulkan pacing evidence is recorded
in the [GPU report](GPU-VIDEO.md#linux-presentation-backpressure-and-audio).
Older macOS callback pacing and physical 144Hz+ scanout remain unverified.

Sources:

- [Apple: window-aligned display link](https://developer.apple.com/documentation/appkit/nswindow/displaylink(target:selector:)).
- [SDL 3.4.16 Metal implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/gpu/metal/SDL_gpu_metal.m).
- Installed SDK declarations: `AppKit/NSWindow.h`, `AppKit/NSScreen.h`,
  `QuartzCore/CADisplayLink.h`. No private SDL layer access or drawable theft.

