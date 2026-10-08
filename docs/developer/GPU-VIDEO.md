# GPU presentation compositor

This developer report preserves successive presentation checkpoints. The FDP
reference and the CPU compositor are MAME-derived compatibility targets, not
chip-verified TC0630FDP models. Tables apply to the named macOS/Metal host and
revision; interpolation is an optional presentation change, not hardware fidelity.

## Scope and checkpoints

Presentation only. Native `Machine::native_pixels()`, strict-native execution, captures,
frame/replay CRCs, audio and rollback snapshots remain CPU-produced. FDP oracle
fallback remains available. The CPU path is retained and row-parallelized before
the GPU port. Checkpoints are distinct commits/tags:
`gpuvideo-1-threaded-cpu`, `gpuvideo-2-gpu-parity`, `gpuvideo-3-interp`.
Interpolation is not part of the parity contract or parity checkpoint.

Supported GPU `Game` frames may defer native CPU composition until
`Machine::native_pixels()` or a snapshot/capture observes it. The immutable
scanout and retained current-frame sprite plane reproduce the same CPU pixels;
CPU presentation, Diagnostic/Compare and unsupported oracle fallback remain
unchanged. This is avoided work, not lower-resolution or approximate rendering.
Measurement and observation-boundary evidence:
[WORKFLOWS.md](WORKFLOWS.md#upstream-integration-and-deferred-native-composition).

## CPU sampling contract (read before shader implementation)

Sources: `runtime/renderer/game/compositor.cpp`, `tiles.cpp`, `lines.cpp`,
`sprites.cpp`, `text.cpp` and [VIDEO-HLE.md](VIDEO-HLE.md).
Let S be internal scale, B native border, W=(320+2B)S,
L=46-B, output column u, native scanout row y=24+floor(v/S),
and sub-row t=v mod S. All layers use **that native row's** controls;
the existing compositor does not interpolate adjacent line-RAM values.

### Playfields and line effects

`GameLines::prepare` normalizes game-owned producer state. PF source X includes
register scroll, signed rowscroll and `10*(x_step-256)`, retaining 24.8 phase.
Y is an accumulated 24.8 register position plus integer per-line column scroll,
wrapped to 512; its fractional byte is retained separately. X step is
`256-zoom_byte` (1..256); Y steps come from the low zoom byte, doubled, with
register-to-layer mapping `{0,3,2,1}`. These are source increments, not display
size multipliers. The normal supported transform has no arbitrary matrix
rotation: apparent perspective/warping comes from per-line scale and offsets.

For sample column q=u+LS:

- X = floor((source_x*S + (q-46S)*x_step)/(256S)).
- F = trunc((y_fraction*S + t*y_step)/S).
- Y = source_y + (F >> 8).

With `full_resolution_alt_maps` (Command War) the same X formula runs on the presented row:
`gpu/encode.cpp` writes `presented_playfield()` (main map, `x_step` up to 512, `source_x` doubled plus
`c << 8`) for expanded output only; the shader needs no change and captured rows stay canonical.

Signed X division is mathematical floor, **not** C/GLSL signed truncation:
`n>=0 ? n/d : -1-(-1-n)/d`. Combined numerators must be divided only once.
Maps wrap X to 1024 and Y to 512, select a 64x32 semantic cell and its 16x16
ROM tile. Cell flips XOR texel coordinates with 15. Pens are masked by the
cell's extra-plane mask; pen zero is transparent. Nontransparent palette is
`cell.palette+pen`; zero palette is rejected before adding the per-line
palette offset, which wraps as a 16-bit addition. Tile code masks to 32767.

### Mosaic and clipping

Mosaic is per-layer horizontal sample-and-hold. If enabled and period P>1,
let h=L+floor(u/S), c=((h+68)%432+432)%432;
replace q with `(h-(c%P))*S`. Subcolumns then share that sample. Sprite/text
mosaic follows the same rule. No new vertical mosaic is invented.

Clip windows are half-open native scanout coordinates, multiplied by S.
The CPU's inverted-window combining rule is retained, including its unusual
`max(range.left,candidate.right)` behavior. Overlapping resulting ranges can
visit the same pixel more than once; GPU parity must preserve those visits,
not replace the algorithm with a simple four-plane Boolean test.

### Sprites: explicitly a separate raster, not PF-style sampling

**Sprites are drawn as a separate indexed plane at the requested internal
resolution, before the compositor samples that plane.** They are not sampled
by applying PF line zoom or interpolating line values. The game descriptors
quantize each tile origin to integer native coordinates; raster X scale masks
low four grid-zoom bits while tile placement retains them. Submitted geometry
has one-frame scanout lag; GPU submission must use the list that produced the
current CPU frame, not the newly latched next-frame list.

For each sprite's 16x16 texel (a,b), normal orientation:

- px=(sprite.x+a*scale_x)*S+128;
  x0=(px>>8)-LS, x1=((px+scale_x*S)>>8)-LS.
- py=(sprite.y+b*scale_y)*S+255;
  y0=(py>>8)-24S, y1=max(y0+1,((py+scale_y*S)>>8)-24S).

X spans of zero width are skipped; Y always covers at least one output row.
Flips XOR source texel coordinates with 15. Pen zero is transparent after
command pen masking. Color is `uint16(0x1000+(palette<<4)+pen)`. Nominal 24.8
sprite rectangle is culled **before** raster rounding: right<=L*256,
left>(365+B)*256, bottom<=24*256, or top>255*256 rejects it. This prevents the
known top-edge leak. CPU visits sprites in reverse list order and writes only
empty destination pixels; larger list indices win. Within a tile, earlier
texel rows win when forced one-row spans overlap. The GPU draws one
integer-aligned quad per sprite in increasing list order. Its fragment shader
inverts these exact spans, skips zero-width X texels, and scans overlapping
Y rows in logical order until the first masked opaque pen. Steps of at least
one output pixel need only one row lookup. Transparent fragments discard;
later sprites overwrite an integer indexed render target. This preserves both
precedence rules without atomics or per-fragment sprite-list scans. The initial
Metal implementation instead emitted 256 reverse-order texel quads per sprite;
the one-quad cutover and Linux evidence are recorded below.

The compositor accepts the stored sprite only for its priority group
`(color>>10)&3`. Each group has its own row clip, mosaic, blend and priority.
Global flip and retained sprite trails remain oracle fallbacks.

### Text, ordering and integer blending

Text samples X=floor((text_x*S+q-46S)/S), Y=text_y (no sub-row Y).
Its 64x64 map and 8x8 glyphs wrap to 512x512. Flips XOR with 7;
pen zero is transparent, palette is `cell.palette*16+pen`.

Stable priority sort is descending, with equal-priority order:
text, SP0, PF0, SP3, PF3, SP2, PF2, SP1, PF1.
Each output sample starts source index/weight/priority=0/0/0,
destination=background/8/0, source mode=255. A layer matching current source
mode is skipped. `mix` preserves the CPU's strict source-priority comparison,
non-strict destination comparison, equal-destination clearing, normal/reverse
weight slot selection and zero-weight rejection. RGB channels are
`min(255,(src*src_weight+dst*dst_weight)>>3)`;
palette indices mask to 8191 and alpha is 255. Hardware blend is disabled.

## GPU design and uploads

Use SDL3 GPU, Metal on this Mac and SPIR-V/Vulkan on supported hosts, not raw
Metal/GL. GLSL shaders are compiled offline to SPIR-V and translated to MSL;
only shader **source** and build tools are committed, never compiled binaries.

Immutable packed-byte PF and sprite graphics are uploaded once. `GameVideo`
keeps one typed `CapturedFrame` (tiles, text, scene rows, palette colors, the
**rendered** sprite list/pen mask) overwritten at each VBSTART; the CPU
compositor reads it directly and `GpuVideo` encodes it once per present
(`runtime/renderer/gpu/encode.cpp`, the only writer of scene words) straight
into the mapped upload buffer. Each frame therefore uploads semantic PF cells,
text cells/raw glyph RAM, RGB palette, normalized row tables (stable priority
order and exact clip ranges) and the sprite list. The buffer layout is defined
once, in `runtime/renderer/gpu/scene_layout.h`, which the encoder and the
shaders (`#include "../gpu/scene_layout.h"`) share. Semantic data avoids a
second FDP-RAM reader.
A small uniform contains scale/border/dimensions and diagnostic layer selection.
GPU resources/transfer buffers are reused and cycled to protect in-flight data.

Pass 1 draws one integer-aligned quad per sprite into an indexed texture,
resolving exact CPU texel coverage in the fragment shader.
Pass 2 is a fullscreen fragment shader: one invocation per internal-resolution
output sample, integer PF/text sampling and CPU mixing, no final-frame stretch.
Pass 3 letterboxes that texture into the swapchain, with requested nearest or
linear final filtering. Offscreen parity uses the same passes 1/2 and explicit
fenced readback; normal presentation does not read back or upload expanded RGB.

Unsupported frames upload only the native oracle framebuffer; the shader
integer-enlarges its center and paints black border columns exactly as the CPU.
Scene payload and backend state are host presentation data, outside machine
snapshots. Headless and capture/CRC code never depend on GPU availability.

## Verification and interpolation follow-on

Record exact final and per-layer mismatch counts across S=1..4, B=0/48,
seeded gameplay and targeted oracle transitions. Record CPU serial/threaded
and GPU timings (mean/p95/worst), separating synchronized compositor latency
from normal whole-frame presentation throughput. Capture frames outside git
for each tagged checkpoint.

After the non-interpolated parity checkpoint, measure adjacent line values in
actual character selection, especially the water pattern's scale and palette
addition. Record all rows and step-size distributions before selecting guards.
The follow-on mode is opt-in, does not interpolate sprites, and does not change
native CPU outputs. No visual gain or smooth-field safety is claimed before
those measurements.

## Threaded CPU checkpoint evidence

Apple M5 (4 performance + 6 efficiency CPU cores, 10 GPU cores, 32 GiB),
Release `-O3`. Three persistent workers plus caller own disjoint contiguous
native-row ranges. Native 320x232 remains serial to avoid dispatch overhead.
The serial entry point and threaded entry point share the same row kernel.

At seed 5, frame 1500, border 48, 300 timed compositor-only repetitions after
30 warm-ups (milliseconds; sprite raster/emulation/upload excluded):

| Scale | Serial mean / p95 / worst | Threaded mean / p95 / worst |
| --- | ---: | ---: |
| 1 | 1.683 / 1.752 / 1.817 | 0.589 / 0.849 / 1.025 |
| 2 | 6.507 / 6.809 / 9.102 | 1.925 / 2.861 / 7.957 |
| 4 | 25.265 / 25.526 / 34.908 | 7.131 / 8.636 / 10.515 |

Scale 4 mean speedup: 3.54x. A strict-native seed-5 smoke through 3600 frames
compared 144 serial/threaded images: scales 1..4, borders 0/48/160, 12 scenes
at 300-frame intervals. **Zero differing pixels**; 48,820,718 native blocks,
zero interpreter fallback. `f3rt-check` passes.
External captures: `/tmp/f3-gpuvideo/threaded-cpu/frame_{1200,1500,3600}_4x.png`.
These are sampled rerasterizations using the live scene at the sample boundary,
not a new claim about scanout lag. Actual Cocoa/Metal frontend additionally
ran 1500 frames at scale 4/border 48, internal 1664x928, nearest, with native
CRC `4fe25eb9`, 23,203,423 native blocks and zero fallback instructions;
its surface PNG was visually inspected.

## Port invariant and fallback evidence

Native output is still composed on the CPU at every VBSTART. GPU export occurs
before `latch_sprites`, so its sprite list is the list rendered in that frame,
not the following list. GPU resources/caches are absent from canonical state.
CPU-only presentation remains the default; GPU selection is explicit.

An independent CPU-backend snapshot branch exposed an expanded trails history
bug: deferring every expanded sprite raster lost intermediate retained sprites.
Seed 5/frame 3404, four induced trail lists with changing scroll, failed before
the correction with **213,384 differing canonical bytes**. The fix seeds the
preceding expanded plane on entering retention and keeps rasterizing while
trails are latched. The same regression now has **zero differing bytes**.
Trail frames remain exact native oracle presentation, not unsupported HLE.

Seed 5 through 3600 frames additionally induced actual bitmap, trails and flip
fallback boundaries at frame 3404, restoring
and replaying the original native frame after each branch. All three fallback
images and supported recoveries match CPU output; canonical restore is exact.
The original 231 startup fallback frames remain unchanged.
Isolated-layer counts exclude fallback images, since fallback bypasses masks.

Fresh headless `--video compare --video-backend gpu` (GPU flag intentionally
does not instantiate a device headlessly) ran 3600 native frames:
3369 supported native RGB comparisons / 250,114,560 pixels / zero mismatches;
51,507,335 native blocks and zero CPU fallback. Retained MAME captures at
600..3480 compare **25/25**, 1,856,000 pixels, zero mismatches/channel error.
Frame-600 main RAM and the complete 3600-frame WAV are byte-equal to the
retained MAME RAM / integration `build/coverage-final.wav` respectively.

Actual SDL GPU/Metal window runs at scale 4/border 48 retain native CRC
`3fadf226` at frame 1920, 28,866,756 native blocks and zero CPU fallback.
Nearest/linear presentation is supported; internal-resolution readback is
saved separately from the native CPU capture. External GPU artifacts are in
`/tmp/f3-gpuvideo/gpu-parity`; no images or generated shader binaries are in git.

## Seeded GPU parity checkpoint

Metal, seeds **5/6/7/41**, all scales **1/2/3/4** and borders **0/48**:
32 runs × 4000 native frames = **128,000 frames**. GPU/CPU comparisons sampled
every 30 frames plus the final frame: **4320 full frames**, comprising
4096 supported scenes and 224 exact native-oracle fallback images.

| Layer | Supported comparisons | Mismatching pixels |
|---|---:|---:|
| PF0 | 4096 | 0 |
| PF1 | 4096 | 0 |
| PF2 | 4096 | 0 |
| PF3 | 4096 | 0 |
| SP0 | 4096 | 0 |
| SP1 | 4096 | 0 |
| SP2 | 4096 | 0 |
| SP3 | 4096 | 0 |
| Text | 4096 | 0 |
| Composite (including fallback) | 4320 | 0 |

Every run has 3769 supported native frames, 231 actual startup fallback
frames, three actual oracle-mode transitions, native sound and **zero CPU
fallback instructions**. These numbers do not claim every emulated frame was
GPU-diffed. Logs and machine-readable totals:
`/tmp/f3-gpuvideo/parity-matrix/{seed*-s*-b*.log,runs.json,summary.json}`.

Both rendering passes are guarded by exact canonical snapshot byte comparisons.
Expanded deferred presentation also has an independent CPU-backend trail-history
branch, rather than relying only on a same-machine checksum.

An allocation-counting smoke caught **11 first-save allocations** in an expanded
GPU scene. Reference storage is now allocated at enablement, and the rare lazy
snapshot materialization uses serial composition rather than starting workers.
At frame 601, scale 1/border 0 and scale 4/border 48 each report **zero first-save
and restore/save allocations**, zero restored byte differences and zero CPU
fallback. Native netplay remains scale 1/border 0; no snapshot schema changed.

## GPU parity checkpoint performance

M5/10-core GPU/32 GiB MacBook Pro, Release `-O3`, SDL 3.4.16, Metal.
One benchmark process at a time; seed 5 through 4000 frames, border 48,
600 native warmup frames. The final build specializes scale 1/2/4 divisions
to shifts and scale 3 to constant division; signed floor and signed truncation
remain distinct. The complete 32-case parity matrix passed again afterward.

**Final supported frame, 100 repetitions after five warmups**, milliseconds:

| Scale | Path | Mean | p95 | Worst |
|---:|---|---:|---:|---:|
| 1 | CPU serial | 1.727 | 1.760 | 1.781 |
| 1 | CPU threaded | 0.522 | 0.794 | 0.798 |
| 1 | GPU upload/submit/fence/readback | 1.246 | 1.669 | 2.599 |
| 2 | CPU serial | 6.905 | 6.993 | 7.049 |
| 2 | CPU threaded | 1.989 | 3.057 | 3.219 |
| 2 | GPU upload/submit/fence/readback | 3.026 | 3.105 | 4.618 |
| 4 | CPU serial | 27.084 | 28.283 | 28.933 |
| 4 | CPU threaded | 7.540 | 8.803 | 11.471 |
| 4 | GPU upload/submit/fence/readback | 6.473 | 7.414 | 8.243 |

**Varied supported frames** (117 timed composites, 116 matched native/render
budgets; the extra composite is the frozen benchmark readback), milliseconds:

| Scale | Path | Mean | p95 | Worst |
|---:|---|---:|---:|---:|
| 1 | CPU threaded compositor | 0.503 | 0.775 | 0.816 |
| 1 | GPU fenced compositor | 1.714 | 1.960 | 2.139 |
| 2 | CPU threaded compositor | 1.945 | 2.995 | 4.118 |
| 2 | GPU fenced compositor | 2.977 | 3.446 | 4.085 |
| 4 | CPU threaded compositor | 6.948 | 8.413 | 10.980 |
| 4 | GPU fenced compositor | 7.112 | 7.496 | 8.393 |
| 1 | Native CPU + threaded render budget | 3.753 | 4.275 | 5.108 |
| 1 | Native CPU + fenced GPU render budget | 4.963 | 5.566 | 6.336 |
| 2 | Native CPU + threaded render budget | 5.228 | 6.388 | 7.872 |
| 2 | Native CPU + fenced GPU render budget | 6.261 | 7.194 | 7.420 |
| 4 | Native CPU + threaded render budget | 10.279 | 11.905 | 14.165 |
| 4 | Native CPU + fenced GPU render budget | 10.443 | 11.082 | 11.720 |

Native timing covers emulation, native CPU video and scene export. The budget
sums those measured phases for the same sampled frame; it excludes PCM mixing,
window blit, vsync, snapshot guards and comparison loops. GPU numbers include
fenced readback, which normal presentation does not perform. They are not
hardware timestamp-query measurements of the shader alone.

Actual scale-4/border-48 nearest GPU **window** execution, `--unthrottled
--frames 3600 --no-audio`, completes in **31.56 seconds / 114.1 frames/s**
including startup and final PNG readback. The normal native audio driver and
PCM rendering still execute (`--no-audio` disables the output device only).
This exercises presentation with headroom over nominal 60 Hz, rather than
inferring window performance from a standalone shader benchmark.
Native frame CRC `3359f200`, 51,507,335 native blocks, zero fallback.
An actual live SDL window screenshot and final internal-resolution images
were inspected.

The same benchmark run also independently replays its final frame on an
ordinary **CPU-backend Machine**, comparing canonical bytes and the GPU image
against the original CPU `presentation()`: zero differences at all three
scales. Full logs: `/tmp/f3-gpuvideo/bench-fastdiv`.
Earlier full-4000-frame timing logs (before division specialization) are retained
in `/tmp/f3-gpuvideo/bench`; their CPU scheduling outliers are not suppressed.
GPU is not made default: native/2x CPU threading is cheaper than a fenced GPU
comparison, and devices without GPU support retain the explicit CPU choice.

## Interpolation input measurements (after the parity checkpoint)
This section records the **historical Phase 5 water-only checkpoint**, not the
current detector. General per-field sampling and native-anchor corrections
are documented in the Phase 7 implementation section below.


Seed 5, frames 1–4000: PF0 source-X varies across rows in 170 frames (the
`0x9ecb0` sine-wave producer); PF2 scale/source-X and palette-add vary in 1368
frames; PF1/PF3 have no corresponding varying fields on this route. The
character-select water is **PF2**, not the separate PF3 character-select
clip/blend setup. Frame 1409 shows the water/stars entrance; frames 1500 and
1560 show the player-select portraits above it. Native/4x captures and complete
256-row field dumps are in `/tmp/f3-gpuvideo/line-profile`.

For the lower PF2 region, screen rows **152–255** (visible rows 128–231):

| Field | Measured profile | Adjacent step |
|---|---|---|
| X scale | 256 down to 50 | −2 |
| Source X (24.8) | 18352 to 51780 at frame 1409 | +76, +332, or +588 |
| Source Y | 128 to 231 at frame 1409; animated phase later | +1 |
| Y scale/fraction | 256 / 0 | 0 |
| Palette add | 640 down to 0 | 0 or −64 |

Palette bands are `(add, length)`: `(640,4), (576,4), (512,4), (448,6),
`(384,8), (320,8), (256,8), (192,12), (128,16), (64,16), (0,18)`.
Screen row 151 has scale 256 but palette add 0; row 152 also has scale 256
but palette add 640. Thus scale-only value-run detection gives the wrong start.

`lines.cpp` already reconstructs the actual `0x9d66a` palette table,
`0x9d72a` scale/centering function, and `0x9d7b6` phase. Its non-flipped origin
152 matches the captured water boundary exactly. It does **not** expose an
active-effect descriptor or raw per-field line-enable bits in `SceneRow`.
The candidate is therefore a presentation-only recognizer of that complete
known producer profile, not a claimed existing descriptor API: require all
104 enabled, non-mosaic rows and the exact scale/palette shape, validate
source continuity and stable layer controls, and reject the entire candidate
if any input is invalid. Only those rows may enter endpoints/fits. Unknown
effects, including PF0's sine, remain unchanged.

The game's centering function implies a smooth source-X slope of 324 in
24.8 units per row; its integer packing leaves at most 182.462 units
(0.713 texels) residual in the captured profile. Candidate limits are
640 for a positive adjacent source-X jump and 192 for affine residual.
The scale function itself is exactly linear (−2 per row).
Source X is also limited to ±2^24 raw units for reliable float precision;
out-of-domain coordinates decline instead of guessing.

Palette addition is a **64-color bank offset**, not a continuous pen index.
Interpolating its numeric index would select unrelated pens. Blend RGB values
of the same pen in adjacent 64-color banks instead. Actual sampled tile pens
at frames 1409/1500/1560 have maximum channel step 8 for bank pairs 1–10, but
bank 0↔1 has a real 144-channel discontinuity. Preserve that discrete boundary:
the continuous palette candidate ends at row 237 (bank 1), excluding rows
238–255 from palette fit samples. A 32-channel continuity limit separates the
observed smooth pairs from the discontinuity. Fit geometry over 152–255 and
palette only over 152–237; preserve both fields' first/last rows on their
outside edges. A cubic fit of this 86-row palette prefix has maximum residual
39.306 bank-index units; candidate residual limit is 48. These are measured
thresholds, not a universal interpolation rule.

The implemented comparison is off, piecewise-linear, and affine-source/cubic-
palette fitted sampling at 4x. Both opt-in methods leave sprites, source Y,
native pixels, machine state and CPU captures alone. The fit is a guarded
smooth approximation, not proof of the ROM palette table's analytic function.

### Boundary candidate comparison

Cheap value-only runs on the captured visible rows give:

| Frame | Adjacent scale delta ≤2 | Scale plus palette delta ≤64 |
|---:|---|---|
| 1200, no PF2 effect | 24–255 | 24–255 |
| 1409 / 1500 / 1560, water | 24–255 | 24–151 and 152–255 |

The combined value threshold recovers the lower water boundary but also
admits an upper run; scale alone joins across it and recognizes a no-effect
scene. The measured producer origin 152 plus the **complete** lower profile
validation is therefore the conservative shipped choice. No persistent
setter hook/state field or generic value-run fallback was added. This avoids
new canonical state and unknown-effect false positives; it deliberately
declines otherwise smooth effects not covered by this one family.

### Visible gain and boundary proof

Off/linear/fit full frames and water details at 1409/1500/1560 were inspected.
The linear mode visibly removes the four-output-row staircase in the water's
perspective streaks. Fitted geometry removes the packed-source jitter and
spreads smooth palette grading across the valid prefix; its visible advantage
over linear is smaller than the off→linear change. Neither adds ROM texture
detail or changes portrait/sprite geometry. The bank-0 transition remains
discrete, and the water horizon has no added smeared/garbage band.

| 4x frame, border 48 | Linear vs off changed pixels | Fit vs off changed pixels | Outside/boundary pixels changed |
|---:|---:|---:|---:|
| 1500 | 91,476 / 1,544,192 (5.92%) | 264,351 / 1,544,192 (17.12%) | 0 |
| 1560 | 82,781 / 1,544,192 (5.36%) | 229,435 / 1,544,192 (14.86%) | 0 |

The off 4x center differs from plain nearest enlargement of native RGB in only
25,668 / 1,187,840 pixels (2.16%) at 1500 and 20,948 (1.76%) at 1560:
extra sampling without line interpolation mostly retains native geometry.
These difference counts are not a quality score. Linear leaves every
native subrow-zero sample exact; fitted GPU presentation also changes 62,661
such samples at 1500 / 54,089 at 1560, while CPU native output remains exact.

An actual Cocoa/Metal window was replayed to seed-5 frame 1500 with fit
accepted (`geometry=152..255`, `palette=152..237`, 22,705,262 native blocks,
zero instruction fallback). Its internal surface and live window screenshot
were inspected, not merely an offline shader image. Captures:
`/tmp/f3-gpuvideo/interpolation/actual-water-{surface,window}-fit.png`.

The harness checks whole first/last geometry rows and every outside row
bit-identically against off on each accepted sampled frame. It compares
entire declined/oracle images, isolated sprite contributions and canonical
scene/machine bytes. Eleven induced scene-copy cases cover garbage and zero
above 152, disabled/zero inside, jumps, nonmonotonic source, a short valid
region, high source-fit residual, corrupt palette shape, a too-short
RGB-continuous prefix and an out-of-domain source offset. Invalid inside
cases decline with zero image differences against off of the same scene;
above-region corruption leaves eligibility, residuals and all run pixels
exact. These are guard fixtures, not claims of played scenes.

### Final cross-mode corpus

After integration, the off shader's full 32-case matrix was rerun: seeds
5/6/7/41 × scales 1–4 × borders 0/48 × 4000 = 128,000 native frames,
4320 composites, 4096 comparisons of **each** isolated layer, zero mismatches.
Linear and fit each ran eleven additional 4000-frame cases: all four seeds at
4x/borders 0 and 48, plus seed 5 at scales 1/2/3 with border 48.

| Mode | Native frames | Sampled images | Applied | No known effect | Oracle | Native-scale skip |
|---|---:|---:|---:|---:|---:|---:|
| Linear | 44,000 | 1485 | 384 | 896 | 70 | 135 |
| Fit | 44,000 | 1485 | 384 | 896 | 70 | 135 |

Every outside/boundary, declined/oracle and isolated-sprite comparison is
exact; 768 accepted sprite-isolation checks and twenty sets of eleven guard
fixtures pass. All same-seed/scale/border off/linear/fit runs have identical
native frame/audio/state CRCs, native cycles and block counts, with zero
instruction fallback. Logs and summary: `/tmp/f3-gpuvideo/interpolation/matrix`.
Counts are sampled every 30 frames plus the final frame, not all native frames.

For each mode at 4x/border 48:

| Seed | Applied samples | No-known-effect samples | Oracle samples | Sampled accepted interval |
|---:|---:|---:|---:|---|
| 5 | 46 | 82 | 7 | 1411–2761 |
| 6 | 45 | 83 | 7 | 1591–2911 |
| 7 | 9 | 119 | 7 | 1741–1981 |
| 41 | 46 | 82 | 7 | 1621–2971 |

The same results hold at border 0. Native-scale requests stay fully off.
Seed-5 frame 1300 is the separate PF0 sine-wave game-select screen; its PF0
source X ranges −52..261688. Both modes decline it with a whole-image exact
comparison. Off/linear/fit captures are in
`/tmp/f3-gpuvideo/interpolation/other-line-effect`. No generic effect coverage
or interpolation on genuinely discrete unrelated profiles is claimed.

The 4000-frame fit run also exercises induced bitmap, trails and flip
fallback/recovery, keeping canonical bytes exact.
A fresh headless `--video compare --video-backend gpu --video-interp fit`
3600-frame run retains 250,114,560 native RGB comparisons with zero differences,
frame CRC `3359f200`, 51,507,335 native blocks and zero instruction fallback.
Its WAV compares byte-identically with both the parity checkpoint and
integration `coverage-final.wav`. The retained `F3RT_GPU=OFF` frontend and
`f3rt-check` also build/run without shader tools.

### Accepted-water performance

M5/Metal, seed-5 frame 1560, 4x/border 48. Separate sequential mode runs;
100 frozen repeats after five warmups. Milliseconds, including CPU metadata
analysis, uploads, submission, fence and readback for GPU rows:

| Mode run | Path | Mean | p95 | Worst |
|---|---|---:|---:|---:|
| Linear | CPU serial | 26.226 | 26.958 | 28.086 |
| Linear | CPU threaded | 7.685 | 9.784 | 11.513 |
| Linear | GPU off | 4.387 | 6.022 | 6.476 |
| Linear | GPU linear | 4.640 | 6.201 | 7.043 |
| Fit | CPU serial | 26.456 | 27.120 | 27.579 |
| Fit | CPU threaded | 7.721 | 9.236 | 11.158 |
| Fit | GPU off | 4.786 | 6.844 | 8.490 |
| Fit | GPU fit | 4.848 | 6.963 | 7.595 |

The selected effect is actually accepted in these frozen timings, unlike the
earlier frame-4000 benchmark where fit declines. GPU clocks/cache/order vary
between runs; these data do not establish that fit is faster than linear.
Full varied timings and unfiltered logs: `/tmp/f3-gpuvideo/interpolation/perf`.
Independent CPU-backend canonical bytes and GPU-off pixels at frame 1560
remain exact in both runs.

Actual frontend `--video-backend gpu --video-interp fit --video-scale 4
--video-border 48 --frames 3600 --no-audio --unthrottled` completes in
**30.31 seconds / 118.8 frames/s**, including startup and final PNG readback.
Native CPU/audio still run: frame CRC `3359f200`, 1,817,655 audio frames,
51,507,335 native blocks, zero instruction fallback. Internal surface inspected:
`/tmp/f3-gpuvideo/interpolation/perf/frontend-fit-4x.png`.

Decision: ship both as separate opt-in modes, off/CPU still defaults. The
measured water gain is worthwhile; broad unknown-effect interpolation is not
justified. Keep the known-family/validity guards rather than infer arbitrary
smooth runs or claim a universally accurate fitted effect function.

## Automatic internal resolution (phase 6)

`--video-scale auto-integer` and `--video-scale auto` require the GPU backend.
Fixed numeric scales retain 1–4 on both backends. The window fit uses
**physical pixels** from `SDL_GetWindowSizeInPixels`, including the native
border width `320 + 2*border`; automatic windows request high pixel density.

- **Auto-integer:** floor the limiting width/height ratio, clamp 1–4, rasterize
  internally at that scale and blit 1:1 with nearest sampling. Center the
  remainder on black; `--video-filter linear` cannot make it fractional.
- **Auto:** ceiling of the limiting ratio, clamp 1–4, rasterize at that scale,
  then use the existing aspect-preserving nearest/linear window blit.
  If the window exceeds 4x, the cap means this is an upscale, not supersampling.
- Below the 1x footprint, auto-integer centrally crops rather than shrinking.
  During debounce the old integer texture can likewise crop in a smaller
  window; the settled scale follows the new fit.
- F11/Alt+Enter toggles fullscreen. Pixel polling covers resize, fullscreen
  and display-density changes without depending on event subtype ordering.
  Changes settle after **100ms quiet / 250ms maximum live-drag delay**.
  A same-scale resize changes the viewport, not resources.

### Runtime resource and state contract

`GpuVideo::set_scale` replaces only sprite/surface targets and any already-used
readback buffer/capture storage. Device, tile/sprite assets, scene/native/upload
buffers, sampler and pipelines survive. SDL releases old resources when queued
users finish: **no GPU-idle wait or asset re-upload**. Both off and interpolation
pipelines are created once, and interpolation tile pen masks are computed even
when startup scale is 1; transitions through 1 cannot lose the effect.

`GameVideo` constructor options remain the canonical CPU snapshot geometry.
`set_gpu_scale` changes separate host/reference geometry. Canonical and selected
diagnostic sprite planes share decoded sources; diagnostics allocate only when
used, then resize once per scale change. Canonical materialization uses its
fixed plane and never resizes the diagnostic plane. Snapshot size/bytes, native
sprite lag, expanded trail retention, CRCs, cycles and audio are unaffected.
Trails still use exact native oracle output at any GPU scale; scale changes do
not clear their history or introduce a one-frame retention glitch.

Frontend changes occur **after native audio is enqueued and before GPU draw**.
The integrated audiosync pre-enqueue 50ms FIFO cap and 50ms late-deadline resync
remain. Headless automatic flags use startup scale 1 without SDL initialization
or window queries. Netplay explicitly rejects automatic modes and retains fixed
scale 1/border 0. No native CPU/sound/snapshot schema changes.

### Maximum-scale decision and frame cost

Keep the production numeric/automatic cap **4**. The diagnostic/render API and
parity harness support 1–8 solely to measure the larger targets. Measured 5x
GPU tail already consumes most of the 16.67ms budget before native CPU and
presentation/interpolation; 6–8x exceed it. A 4K auto window therefore scales
the 4x image; auto-integer retains centered 4x. No implicit performance-adaptive
mode or change to the numeric CPU flag.

M5/Metal, Release, border 48, seed 5, supported water frame 1560. One process at a
time, 100 frozen samples after five warmups. GPU includes upload/submission,
fence wait and readback, not swapchain/vsync. Native CPU columns are separate
960-frame post-600 samples, not a sum of percentile ranks.

| Scale | Internal | GPU mean/p95/worst ms | Native CPU mean/p95/worst ms |
|---:|---|---|---|
| 1 | 416×232 | 1.272 / 1.701 / 3.441 | 3.425 / 3.821 / 4.441 |
| 2 | 832×464 | 2.897 / 5.085 / 6.553 | 3.475 / 3.939 / 4.102 |
| 3 | 1248×696 | 4.981 / 6.086 / 7.601 | 3.399 / 3.777 / 4.062 |
| 4 | 1664×928 | 5.998 / 7.631 / 9.985 | 3.415 / 3.800 / 4.098 |
| 5 | 2080×1160 | 8.318 / 13.044 / 14.552 | 3.415 / 3.729 / 4.321 |
| 6 | 2496×1392 | 11.332 / 15.154 / 16.615 | 3.398 / 3.764 / 4.172 |
| 7 | 2912×1624 | 11.248 / 17.873 / 23.675 | 3.488 / 3.988 / 4.844 |
| 8 | 3328×1856 | 12.336 / 18.251 / 23.673 | 3.472 / 3.958 / 4.898 |

All eight runs also perform CPU/GPU parity, oracle readback and an independent
CPU replay with zero differences. Scales above 4 use a fixed scale-1 canonical
machine and the selected host CPU-reference geometry. CPU serial/threaded and
varied matched-budget tables remain in the logs:
`/tmp/f3-gpuvideo/auto-scale/perf/scale{1..8}.log`.

### Transition and native-invariant corpus

27 runs ×4000 = **108,000 native frames**: fixed scales 1–4/borders 0/48,
changing scales for seeds 5/6/7/41 at both borders, unchanged scale-1 baselines,
linear/fit changes for seed 5 at both borders, and one every-frame changing run.
The sequence is `1→3→2→4→1→3→2→4→1` at frames
240/1400/1500/1600/2000/3000/3600/3900. Changes force comparison even off the
sampling interval; induced bitmap/trails/flip/writer/ending branches are restored
without native state changes. Four retained trail frames additionally change
`3→2→4→1` and compare an independent unchanged CPU-backend snapshot.

**7188 full composites**, **2160 comparisons of each of nine isolated layers**,
zero differing pixels, zero instruction fallback. The every-frame run checks all
4000 gameplay images plus induced branches. Off/linear/fit changing runs have
identical final machine/audio/state CRCs, cycles, native block counts and complete
audio counts to their same-seed/border fixed scale-1 baselines. Each individual
scale change also asserts identical canonical snapshot bytes at the frozen frame.

Fresh ordinary headless CLI with auto-integer/border 48/fit flags retains
**250,114,560 native RGB checks, zero mismatches**, frame CRC `3359f200`,
51,507,335 native blocks, zero fallback and byte-identical established
1,817,655-frame WAV. CPU-only `F3RT_GPU=OFF` build, runtime device check,
CPU window and clear automatic-mode rejection also pass.

Reproduction:

```sh
./build/f3rt-gpu-regression --seed 5 --frames 4000 --border 48 --every 1 \
  --change-scale 240:3 --change-scale 1400:2 --change-scale 1500:4 \
  --change-scale 1600:1 --change-scale 2000:3 --change-scale 3000:2 \
  --change-scale 3600:4 --change-scale 3900:1 \
  --inject-frame 1501 --inject-bitmap --inject-trails --inject-globalflip
./build/f3rt-gpu-regression --seed 5 --frames 1560 --every 120 --scale 8 --border 48 --bench
```

Full per-run summaries/logs and scale-change captures:
`/tmp/f3-gpuvideo/auto-scale/parity/{runs,summary}.json` and `parity/captures`.
Ending coverage remains an induced producer boundary, not a played ending.

### Physical-pixel viewport examples

Values exercised by the production policy with exact-fit/one-pixel-crossing,
undersized crop and 4K-cap boundary assertions. Rectangles are `x,y,width,height`
in window pixels. Integer is always nearest; auto obeys the selected filter.

| Border | Window pixels | Integer scale / rectangle | Auto scale / rectangle |
|---:|---|---|---|
| 0 | 640×480 | 2 / 0,8,640,464 | 2 / 0,8,640,464 |
| 0 | 1280×720 | 3 / 160,12,960,696 | 4 / 143,0,993,720 |
| 0 | 1920×1080 | 4 / 320,76,1280,928 | 4 / 215,0,1489,1080 |
| 0 | 2560×1440 | 4 / 640,256,1280,928 | 4 / 287,0,1986,1440 |
| 48 | 640×480 | 1 / 112,124,416,232 | 2 / 0,62,640,356 |
| 48 | 1280×720 | 3 / 16,12,1248,696 | 4 / 0,3,1280,713 |
| 48 | 1920×1080 | 4 / 128,76,1664,928 | 4 / 0,5,1920,1070 |
| 48 | 2560×1440 | 4 / 448,256,1664,928 | 4 / 0,6,2560,1427 |

### Actual Mac window, debounce and audio pacing

The actual frontend runs through 1800 strict-native frames in each mode, with
real SDL/Cocoa window-size calls and F11 events (no replacement renderer).
Both select from **2x-density physical pixels**, not points: 320×240 points
reports 640×480 pixels; 640×360 points reports 1280×720. Requested
640×480/1280×720/1920×1080/2560×1440 pixel sizes are observed exactly.
Fullscreen is observed at **3024×1898 pixels**, followed by restored 1920×1080.
Water windows and fullscreen/back surfaces are captured and visually inspected.
Integer's 4x image remains centered on black even with `--video-filter linear`;
auto fills only the aspect viewport.

Six alternating resize calls over five frames settle to the starting scale
with **no extra resource change**. Each run has only four actual scale changes;
live setter durations are **0.031–0.080 ms**. Fullscreen and same-scale resizes
log re-evaluation without recreating resources.

Native CPU/audio continue. Both modes retain frame CRC `fb9bec22`,
26,271,485 native blocks, zero instruction fallback and byte-identical
908,827-frame WAVs. Four external capture pauses plus a deliberate 250ms
external stall exercise late-clock recovery: **9 resyncs** per mode;
FIFO backlog drops **0 / 1**, observed queue mean **32.212 / 30.305 ms**,
maximum **61.792 / 61.826 ms**. The cap is checked before enqueue, so the
post-enqueue statistic can exceed 50ms by one native audio frame; no seconds-long
backlog is retained. No auditory listening claim is made.

Artifacts: `/tmp/f3-gpuvideo/auto-scale/windows/{auto-integer,auto}.log`,
actual `*_1510`, `*_1620`, `*_1690`, `*_1730` window PNGs, internal PNGs and WAVs.
Runtime verification is Metal on this Retina Mac. No second-monitor density
transition or other GPU host is physically exercised; current-pixel polling
uses the same path for those changes.

Frozen native frame 1500, border 48, off shader: **25 measured transitions to
each scale** after one warm cycle, using `1→3→2→4→1` repeatedly. Every first
resized image is fenced/read back and compared exactly with the selected CPU
reference; all 104 first images match and the final canonical snapshot bytes
are unchanged. These costs include targets/readback replacement, not a native
CPU frame, and deliberately differ from the steady frame-1560 workload above.

| Target scale | Setter mean/p95/worst ms | Setter + first fenced image mean/p95/worst ms |
|---:|---|---|
| 3 | 0.031 / 0.041 / 0.045 | 2.926 / 4.981 / 6.248 |
| 2 | 0.028 / 0.041 / 0.051 | 1.965 / 3.192 / 4.270 |
| 4 | 0.037 / 0.053 / 0.068 | 4.304 / 7.608 / 8.800 |
| 1 | 0.033 / 0.067 / 0.067 | 1.201 / 1.712 / 2.351 |

Observed output: `/tmp/f3-gpuvideo/auto-scale/scale-latency.log`.

## General-effect survey (phase 7, docs/data checkpoint)

Survey starts **after** commit `89aa284` / tag `gpuvideo-5-auto-scale`.
This checkpoint changes documentation/data only; the executable remains the
Phase 6 water-only interpolation baseline. General line sampling and sprite
sampling have separate subsequent checkpoints.

### Corpus, row dumps and actual scene coverage

Strict native main/sound, canonical scale 1/border 48, selected GPU scale 4:
seeds **5/6/7/41 ×40,000** plus **12,000 no-input attract frames** =
**172,000 frames**, **170,845 supported**, **1155 startup oracle frames**,
zero fallback instructions. No frame/time/state manipulation in these routes.
Count field variation on every enabled visible row of every frame. Dump all
256 rows of PF0–3, SP0–3 and text every 120 frames and at selected captures,
including source phases/steps, palette add, priority/blend/clip selectors,
four raw clip planes, four saturated alpha weights, mosaic and bitmap state.

An additional five replay/capture passes total 54,600 overlapping frames,
excluded from the share denominators. Actual captures include title/attract,
how-to, game/character selection, several played match backgrounds and
**WON/LOST** transitions: seed 5 frame 4850, seed 7 frame 4250 and seed 6
frame 14030 were visually inspected. Late captures return to the attract demo,
not a played campaign ending. Ending hooks are audited below, but no campaign
ending is reached in these routes.

Artifacts stay outside the repository:

- `/tmp/f3-gpuvideo/general/survey-runs.json`, `survey-summary.json`,
  `{seed5,seed6,seed7,seed41,attract}.log`.
- `survey-data/<route>/{frames,rows,sprites,captures}.csv`: exact every-frame
  field masks/counts and sampled per-row/descriptor dumps. The row CSV includes
  disabled and blanking rows; they are **not** counted as valid effect inputs.
- `survey-data/<route>/frameN.{off4.png,native.bmp,before.state}`: original
  4x surfaces, native captures and untouched pre-scanout snapshots for replay.
- `results-data/<route>` and `results-runs.json`: targeted actual result frames
  with the same per-layer row dumps and replay snapshots.

Work-RAM words at `$401f54/$401f5c` are recorded as **raw mode/phase**, not
trusted human scene labels. Values 2/3/4/7 occur in visibly played matches;
`$ffff` occurs in no-input demos. Calling these a simple charsel/game enum
would mislabel the census. Captures and producer/field signatures label effects.

### Effects by frame share

Counts are frames with at least two unequal values among enabled visible rows.
They are not disjoint; raw alpha/clip planes are shared row state and can be
inactive for a particular layer. Counts do not prove a smooth run.

| Route | Frames | PF0 sine source-X variation | PF2 X-step/source-X/palette variation | Alpha row-block variation |
|---|---:|---:|---:|---:|
| Seed 5 | 40000 | 170 | 1368 | 1288 |
| Seed 6 | 40000 | 332 | 1368 | 982 |
| Seed 7 | 40000 | 476 | 277 | 1594 |
| Seed 41 | 40000 | 374 | 1368 | 982 |
| No-input attract | 12000 | 0 | 0 | 612 |
| **Total** | **172000** | **1352 (0.786%)** | **4381 (2.547%)** | **5458 (3.173%)** |

PF1/PF3 have **zero** varying source-X, X/Y step, Y fraction or palette-add
frames on these routes. PF2 column-offset halves vary in 4373 frames; PF3's
spill at row 48 varies in 4337. PF3 clip-selector blocks vary in 256 frames;
raw clip-plane-3 edges vary in 248. Priority changes are explicitly discrete:
PF1 1683, PF2 5338, PF3 256; SP0/1/2/3 752/496/21645/21645 frames.
All four PFs have **Y step 256 and Y fraction 0** throughout the enabled,
visible sampled corpus; no vertical zoom ramp is observed. Mosaic enables never
vary and no active nontrivial mosaic is observed.

### The scaled floor is not the water

The played board's diamond/perspective **ground texture is PF0**, demonstrated
by isolated GPU/CPU-exact PF0 capture at seed 5 frame 6000. PF1 contains the
surrounding architectural backdrop; PF2/PF3 are empty on that frame. The ground
art itself is pre-drawn perspective in ROM tiles, not a per-line perspective
zoom. Its normalized X/Y steps are 256, source-X is row-constant and source-Y
advances by one. At frame 6000 **every isolated layer and the complete 4x image
have zero differences from nearest-enlarged 1x**. There is no extra source
detail to recover there without inventing texture or changing the transform.

The separate animated water/star backdrop during selection **is PF2**:
`$9d66a/$9d72a/$9d7b6`, X-step 2–256, centering source-X, two column-offset
halves and an RLE palette gradient. The old “water/puzzle-board floor” naming
conflated two visually different surfaces. General sampling must handle the
PF0 wave and both valid PF2 ramp halves, not manufacture a transform for the
ordinary unit-scale PF0 floor.

Isolated images: `survey-data/seed5/frame6000.before.state.layers/{pf0,pf1}.png`.
Measured zero-gain and zoomed-sprite baseline:
`/tmp/f3-gpuvideo/general/frozen-survey-gains.log`.

### Producer and boundary catalog

| Producer | Layer/field | Data/formula and boundary |
|---|---|---|
| `$5cd8`, ROM profile `$5d74` | All layer controls, alpha, mosaic, X/Y step | Visible default profile; X step `256-highByte`, Y step `2*lowByte`, cross-PF Y mapping `{0,3,2,1}`. Default/reset is not an effect endpoint. |
| `$136e` | PF/text global scroll | Actual integer/fractional register conversions; frame-level movement is not a line ramp. |
| `$9d72a` | PF2 X zoom/rowscroll | Symmetric about 151/152; first steps at both center rows are 256, then change by 2 per row. Eight-bit wrap is a hard boundary. Centering conversion uses division/remainder at `$9d774..$9d7aa`, not an assumed floating formula. Normalized adjacent source-X steps include ±76/±332/±588 (24.8). |
| `$9d7b6` | PF2/PF3 column offset | Phase `$40790a&127` after task wake; two constant PF2 halves at row 152, plus PF3 rows 0–47 from the uploader spill. These are discrete offsets, not a vertical ramp. |
| `$9ecb0` | PF0 sine rowscroll | Signed-byte phase into actual 32-bit ROM table `$1c84`, low-word negation, shift/swap and phase +2 per line. Source-X wraps at the 1024-texel map period. Interpolate the **packed table samples**, never replace them with analytic sine. |
| `$9d66a`, table `$9d6a8..$9d6d8` | PF2 palette add | 11 held bands: lengths 4/4/4/6/8/8/8/12/16/16/18, offsets 640→0 by 64. Same-pen RGB blending is a separate optional color invention; actual bank discontinuities remain raw. |
| `$91490/$91506/$915d2` | Text/PF3/SP priority/clip/blend | Rectangular selection label/water row blocks; PF3 water rows 176–251, fixed alpha `{5,4,3,4}`. Preserve row-block topology and priority switches. |
| `$91834` | Clip plane 3 | Held edge across rows 176–251, expanding by 6 **per frame**, not a per-line edge ramp. Native half-open clipping stays discrete. |
| `$98dba..$9ad3e`, `$8cfba/$8cfe0` | Alpha, mix and sprite priorities | Uniform row blocks, frame-time fades and saved-alpha restore. Field variation across block borders is not an alpha gradient. |
| `$fe620/$fefe6/$ff0fa` | Ending bitmap/slides | Audited producers explicitly unsupported by semantic lines; exact oracle fallback, no interpolation. Not exercised as a played ending. |

`runtime/renderer/game/lines.cpp` is the literal semantic producer and normalizer;
[game-data video investigation](VIDEO-HLE.md) and site developer `lines.md` document the source addresses.
Preparation preserves current-row Y phase and advances the accumulator by that
row's Y step. A source-Y discontinuity relative to this advance identifies the
column-offset boundary; a texture wrap alone does not.

### Y's field checklist — evidence, not blanket safety

Survey decisions here describe the intended general cutover, not extra runtime
features already present at the survey tag.

| Field | Used by this game / observed scene | Survey decision | Why / edge cases |
|---|---|---|---|
| Clip edges | Yes: rectangular charsel labels/water, held per row block | **Keep discrete on this corpus** | Zero four-row active edge ramps in all sampled routes; moving edge is temporal. Do not blend mask bits or bleed across rectangle/topology changes. |
| Alpha | Yes: attract/selection fades and dim/restore | **Keep native/discrete** | 5458 varying block frames, but zero four-row alpha ramps; weights are 0–8, constant within blocks. Preserve priority/mix ordering. No invented sub-line alpha weights. |
| Mosaic | Decoded; no active animation observed | **Discrete** | Quantized integer cell size/alignment, not a continuous source coordinate. No evidence supports fractional mosaic. |
| Horizontal zoom | Yes: PF2 symmetric water transform | **General per-layer smooth runs** | Affine ±2 steps; exclude wrap, disabled/invalid rows and hard controls. Keep each native subrow-zero sample raw. |
| Vertical zoom | Decoded; all sampled enabled Y steps 256 | **Existing finer subrow sampling; no extra ramp claimed** | Native phase/Y step already sampled at scale N. No observed line ramp to fit; column jumps must not become zoom. |
| Rowscroll | PF0 table wave and PF2 packed centering | **General sampled-coordinate runs** | Periodic map-phase unwrapping and bounded local linear/fit curves preserve the ROM samples, extrema and discrete jumps. No guessed sine, water-only row range or layer number. |
| Column scroll | PF2 halves, PF3 spill | **Discrete offsets** | Two producer-written constants with real boundaries at 152/48, not a smooth per-line family. |
| Palette add | PF2 RLE banks | **Separately switchable RGB blend** | Same pen in checked compatible banks only. Bank/pen identity, unsafe RGB jump and invalid endpoints remain discrete; outputs can be absent from the native palette. |
| Priority / blend selector / enable | Attract/selection/results blocks, all groups | **Discrete** | Integer topology/order; never continuous endpoints or fit samples across a change. |

### Sprite precision and honest baseline gain

Across the 172,000-frame census, **32,571,398 sprite records**, **259,072 zoomed
records (0.795%)**, and **zero fractional X/Y origins**. Counts include
offscreen/occluded records, not just visible pixels. ROM `$4688` single tiles
retain 8-bit X/Y zoom; `$480c..$4a36` and `$a913c..$a93a2` grid widths mask the
low four X-zoom bits while placement uses all bits. Intermediate half-pixel
carry is quantized to integer tile origins before descriptor submission.
Preserving discarded carry previously caused the frame-1080 native mismatch;
it is not honest subpixel positioning data.

Descriptors support axis-aligned scale and flips, **no angle/rotation matrix**.
Retain the one-frame rendered sprite lag, reverse descriptor/texel precedence,
nominal culling before `+255` Y rounding and the existing off raster.

Existing off 4x already gains source sampling on zoomed sprites: frame 1080
SP2 differs from nearest 1x by **2560 pixels**, SP3 by **8832**, composite by
**8832**, with zero new sampled RGB values in those images. Frame 6000's unit
sprites differ by **zero**. Generalization must distinguish that existing gain
from any newly measured sprite path; do not claim new art, fractional native
motion, rotation or unscaled-texture detail.

### Palette invention and native-anchor baseline

Frozen seed 5 frame 1500, 4x/border 48, 1834 distinct colors among all 8192
**active native palette entries**. For each rendered RGB, measure Euclidean
distance to the nearest of those real entries (8-bit RGB units); mean is
pixel-weighted over non-entry colors. This compares the active game palette,
not an invented color-space curve or only the current two water banks.

| Existing Phase 6 mode | Changed pixels vs off | Changed native subrow-zero pixels | Non-entry RGB pixels / colors | Nearest-entry mean / max |
|---|---:|---:|---|---|
| off | 0 | 0 | 0 / 0 | 0 / 0 |
| linear | 91476 | 0 | 18745 / 339 | 4.543 / 6.928 |
| fit | 264351 | 62661 | 228892 / 895 | 3.880 / 6.928 |

Canonical snapshot bytes are unchanged and instruction fallback is zero.
The old fit changes **62,661 native subrow-zero pixels**: its absolute fitted
source/palette value is not anchored to the packed native row. The new Phase 7
contract requires those samples exact; the general fit must be locally anchored,
not simply broaden the old absolute water model.

RGB blending visibly removes water color-band steps but is quantitatively a
different operation from source-coordinate sampling. Keep palette blending a
separate control; compare geometry-only against geometry+palette after the
general cutover before deciding its default. Native alpha already blends RGB in
other scenes; the survey finds no genuine sub-line alpha ramp to interpolate.
Evidence: `/tmp/f3-gpuvideo/general/survey-data/seed5/frame1500.before.state.{off,linear,fit}.png`.

## General line sampling implementation (Phase 7)

The water-only recognizer and absolute fitted source/palette curves are
removed. `runtime/renderer/gpu/interp.cpp` analyzes all four playfields independently,
over the complete visible scanout. This is host presentation analysis:
no raw FDP reads, game-producer changes, serialized fields or CPU rendering
changes. The uploaded scene retains its canonical words and appends 13 words
per row/playfield (`analyze_gpu_interpolation` returns typed
`InterpolationCoefficients`; `encode()` writes them): flags plus four triples of local
polynomial increments.
Both the storage buffer and cycled staging upload include the entire appended
region; interpolation-off retains the original smaller allocation.

### Valid runs and field controls

- Numeric/enabled validation excludes disabled, zero/garbage, bitmap and
  mosaic rows. Blend/clip/control changes and non-continuous vertical phase
  split runs; the current row's Y step must predict the next packed phase.
- Source X uses shortest map-phase deltas modulo 1024 texels, bounded to
  4096 raw 24.8 units per row (measured steps peak at 588); X zoom deltas are
  bounded to 8 (actual PF2 slope magnitude 2). Four real neighbors and multiple
  changing steps are required. No hard-coded playfield number or water band.
- `linear` evaluates the measured next value. `fit` uses harmonic-mean local
  Hermite tangents, zero at sign changes, then clamps to the endpoint range.
  The polynomial is an increment from the current raw sample. At subrow zero
  the original integer path is used, **including fit**. Run edges remain raw.
  PF0's table wave is sampled-coordinate interpolation, not a guessed sine.
- Varying vertical zoom can use a validated accumulated-phase run; constant
  nonunit Y steps already have finer sampling in the off path. The real survey
  found no varying Y zoom. Column-scroll offsets never become fitted zoom.
- `--video-interp-fields none|geometry|palette|geometry,palette` defaults to
  **geometry**. `none` is image-identical to off. Palette is independent:
  it cannot disable safe source/zoom geometry. Alpha has no pretend smoothing
  flag; all observed alpha/clip/mosaic/priority changes remain native/discrete.
- Palette runs require compatible 16-aligned bank strides and monotone multiple
  bank steps. Every traversed same-pen bank pair is checked against actual ROM
  tile pen sets over all endpoint source/zoom combinations, including border
  and fractional taps. Per-channel RGB jumps above 32 remain discrete.
  Palette blending is RGB interpolation, not fractional pen/index selection.

Per-layer diagnostics report source/zoom/vertical/palette row counts, invalid
rows, discontinuities and unsafe palette pairs. Declines are local to the
uncertain field/run; no invalid neighbor contributes to a cubic derivative.
At seed 5 frame 1500, PF2 accepts 226 source and zoom rows across both halves,
with 9 compatible palette transitions and one unsafe bank pair. The center
column-phase jump stays discrete. PF0 frame 1300 now gains real sampling.
Sprites remain the prior exact off renderer at this checkpoint.

### 4x captures and color decision

Frozen actual native scenes, border 48, 1664x928. Counts compare whole RGB
images with the same-scale off image; all native subrow-zero and unflagged
per-layer rows, text/sprites and canonical machine bytes remain exact.

| Scene | Linear geometry changed pixels | Fit geometry changed pixels | Decision |
|---|---:|---:|---|
| PF0 sampled wave, seed 5 frame 1300 | 49,635 | 49,558 | Smooth the actual packed coordinates; no new RGB values |
| PF2 entrance/column boundary, frame 1409 | 127,706 | 127,595 | Both valid halves; column jump raw |
| PF2 character-select water, frame 1500 | 85,792 | 86,231 | Geometry alone removes line-scale/source stair steps |
| PF2 water, frame 1560 | 75,213 | 75,352 | Same validated field/run treatment |
| PF0 prewarped game floor, frame 6000 | 0 | 0 | ROM texture perspective; no live transform to invent |
| Attract/alpha blocks, frame 2400 | 0 | 0 | Native held alpha/clip blocks |
| Played results, seed 5 frame 4850 | 0 | 0 | Native discrete controls |

Geometry-only captures above contain **zero** RGB values absent from the
current native palette. This is not a universal claim that native alpha
composition can never blend colors: no additional alpha interpolation occurs.

At frame 1500, separately enabling palette with geometry changes 98,727 pixels
for linear and 99,144 for fit. Both add 339 non-palette RGB colors, occupying
18,745/18,746 pixels. Pixel-weighted Euclidean RGB distance to the nearest
of all 8192 current native palette entries is mean 4.543/3.407, worst 6.928
(8-bit channel units). Palette-only changes 18,684 pixels, with means
4.541/3.406. Fit's bounded bank transition is gentler, but the 4x inspected
images do not justify silently inventing colors by default: **geometry on,
palette off** for both opt-in interpolation modes. Interpolation itself still
defaults off. Alpha/clip/mosaic/priority/column offsets remain discrete for
the measured reasons in the survey checklist, not because they are water-free.

External captures and metrics: `/tmp/f3-gpuvideo/general/effect-runs.json`,
`survey-data/*/frame*.before.state.general/*` and
`results-data/seed5/frame4850.before.state.general/*`. They include off, linear/
fit geometry-only, palette-only, combined and none, with isolated PF outputs.
No ROMs, snapshots, helper programs or captures are committed.

### General-line exactness and frame times

The integrated corpus has 56 runs: 224,000 native frames, 3112 whole-image
CPU/off-GPU comparisons and 2544 comparisons in each of nine isolated layers.
All mismatching-pixel counts are zero. Off covers scales 1–4 and borders 0/48;
linear/fit cover those dimensions, geometry/palette/both/none, seeds 5/6/7/41,
and 136 actual host scale changes. Forty opt-in cases add 2248 complete
native-anchor/unflagged/per-layer checks. The 252 uncertain-edge guard checks
use visible real ROM texels, including disabled/zero/garbage neighbors, source/
control/column jumps and unsafe palette pairs: rejected smoothing is checked
in rendered pixels, and unsafe palette must retain visible safe geometry.

For each seed, native cycles, native block count, audio frame count, audio CRC
and final native RGB CRC match across every field/mode/scale. Canonical
snapshots match for the same constructor geometry; runtime scale transitions
leave those constructor-fixed bytes unchanged. Each case independently
replays the final frame on the ordinary CPU backend. Bitmap, trails and global
flip fallback/recovery are induced. These are branch proofs. No CPU fallback.

Fresh 3600-frame headless `compare`, GPU/fit/combined flags and automatic scale
retain 250,114,560 exact native RGB comparisons and byte-identical WAV against
the established native baseline. `F3RT_GPU=OFF` frontend and existing
`f3rt-check` build/run; the device check passes. Native/MAME acceptance remains
the earlier 25/25 proof; no guest/video/audio implementation changed here.

Frozen seed 5 frame 1560, border 48, **100 measured draws after 25 warmup**,
isolated from the corpus/window processes. Milliseconds mean / p95 / worst:

| Scale | CPU reference | Off GPU | Linear geometry GPU | Fit geometry GPU | Fit geometry+palette GPU |
|---|---|---|---|---|---|
| 1 | 1.819 / 3.146 / 6.591 | 1.155 / 1.799 / 2.167 | 0.938 / 1.200 / 1.719 | 0.934 / 1.268 / 1.720 | 0.953 / 1.298 / 1.695 |
| 2 | 6.955 / 10.023 / 15.343 | 1.106 / 1.353 / 1.809 | 0.991 / 1.264 / 1.409 | 0.921 / 1.076 / 1.274 | 0.948 / 1.124 / 1.349 |
| 4 | 21.496 / 29.055 / 30.559 | 2.008 / 2.294 / 2.434 | 2.119 / 2.418 / 2.479 | 2.162 / 2.434 / 2.527 | 2.169 / 2.451 / 2.590 |

These GPU times include scene upload, host analysis, render, download, fence
and CPU readback, not just GPU kernel time. Scale 1 has no interpolation;
different scale-1 means reflect scheduling/order, not a smoothing speedup.
CPU reference costs are only presentation raster costs, not guest execution.
For the newly accepted PF0 wave at frame 1300, scale-4 CPU is
22.777/29.728/37.922; off GPU 2.037/2.363/2.655, linear geometry
2.122/2.393/2.449 and fit geometry 2.179/2.446/2.637 ms.
Do not compare separate historical runs as a hardware-independent speedup.

Actual Cocoa/Metal frontend runs use the real key-event path and `compare`;
off, linear geometry and fit+palette show both PF0 and PF2 acceptance, with
identical final native RGB CRC, cycles/block counts and byte-identical WAVs.
Those diagnostic runs are unthrottled; their intentional audio queue growth
is **not** a production pacing claim. Current-color/source sampling was
visually inspected in the external window/internal-surface captures.

A separate **paced** automatic-integer fit/geometry run exercises a 2496x1392
physical-pixel Cocoa window, internal 1664x928, nearest centered integer
presentation. Over 1600 frames it retains the same native CRC/cycles/blocks
and byte-identical WAV, with audio queue mean 27.984 ms/max 37.499 ms, zero
queue drops and four clock resyncs including screenshot stalls. The actual
automatic-size surface/black remainder was inspected; captured diagnostic
window runs are not substituted for an unobserved second-monitor GPU test.

Reproduction and full logs: `/tmp/f3-gpuvideo/general/parity/{runs,summary}.json`,
`general/bench-runs.json`, `survey-data/seed5/frame*.general-bench.log`
and `general/windows/*`. No changed game state, CPU ABI, netplay snapshot
schema or palette/texture asset data; Metal on this Mac is the exercised GPU.

## Sprite sampling and precision (Phase 8)

Sprite zoom already reaches the internal-resolution grid in the CPU oracle
and GPU sprite pass. For source texel `(a,b)`, scale `N`, descriptor 24.8 origin
`(x,y)` and raster steps `(sx,sy)`, the existing unflipped equations are:

```text
px = (x + a * sx) * N + 128
py = (y + b * sy) * N + 255
x0 = (px >> 8) - origin_x * N
x1 = ((px + sx * N) >> 8) - origin_x * N
y0 = (py >> 8) - origin_y * N
y1 = max(y0 + 1, ((py + sy * N) >> 8) - origin_y * N)
```

The multiply occurs **before** native fixed-point rounding. Boundaries resolve
at `1/N` native pixel, exposing ROM texels lost by a 1x sprite raster. This is
not an enlarged 1x sprite bitmap, and it applies to every supported X/Y zoom
and flip regardless of the line-interpolation setting. Unscaled tiles naturally
produce identical repeated pens. Descriptor origins are already integer native
positions: restoring intermediate producer carry would invent different
placement, not reveal a submitted fractional position.

No additional sprite resampler is justified just by producing a different
image. Floating texel edges, rescaled `+128/+255` phases, removal of minimum
Y splats, bilinear RGB filtering or temporal motion interpolation would change
coverage/native precedence or invent colors, not expose a missing descriptor
field. Preserve nominal bounding-box culling before Y rounding, reverse
descriptor/texel overlap order, transparent-pen handling and the existing
same-scale native sample anchors. `linear`/`fit` describe line-RAM field
interpolation; they do not invent a sprite scale curve or rotation matrix.

Per-row survey/result CSV files under the external evidence directories were
losslessly compressed to `.csv.gz` after disk pressure. Recorded metrics,
snapshots, captures and command JSON are unchanged.

### Sprite exactness, gains and limits

Phase 8 is a verification/diagnostic checkpoint, **not a new sprite sampling
algorithm**. The existing N-grid raster consumes every supported descriptor
field. No fractional origins or rotation matrix were found; there is no
additional supported field to interpolate. The shader now states its
multiply-before-rounding/constant-phase invariant explicitly.

Frozen actual seed-5 frames 1080, 3404 and 6000 were replayed with native main
and sound code. At scales 1–4, borders 0/48, all nine isolated contributions
and composite match the existing CPU raster/compositor: 240 exact comparisons.
The reconstructed scratch CPU reference also matches the public CPU reference
at canonical border 48 in 120 comparisons; 96 original-descriptor subset
comparisons cover unit, minified, enlarged and offscreen/partial categories.
Enlarged subsets are honestly empty in these scenes. All 384 isolated
linear/fit geometry/both sprite comparisons match same-scale off, including
native sample rows. Full canonical scene/machine bytes, state CRC and frozen
native replay remain exact, with zero fallback instructions.

| Actual frame/layer, 4x border 48 | Changed pixels versus nearest 1x | RGB colors, 1x → 4x | Sampled indexed colors, 1x → 4x |
| --- | ---: | ---: | ---: |
| 1080 SP2 | 2560 | 41 → 41 | 40 → 40 |
| 1080 SP3 | 8832 | 64 → 64 | 77 → 77 |
| 3404 SP3 | 2149 | 109 → 109 | 127 → 127 |
| 6000 SP0/SP1/SP2/SP3 | 0 / 0 / 0 / 0 | 57 / 1 / 83 / 128, unchanged | 57 / 0 / 87 / 156, unchanged |

RGB counts include background; indexed-color counts are enabled/clipped/
mosaic-adjusted compositor inputs before blend weights, not new artwork.
The 4x raster resolves source-texel coverage more finely but adds no palette
colors in these samples. Changing from 1x to 4x also changes 105/368/93 native
lattice pixels in those three zoomed layers because the original raster phases
are evaluated on different grids. This is existing CPU behavior, **not** a
claim that every cross-scale anchor equals 1x. Enabling line interpolation
changes zero sprite pixels/native rows at the *same* scale. Unscaled descriptor
subsets in all three scenes have zero changed pixels versus nearest 1x.

`f3rt-gpu-regression --inject-sprite-boundaries` submits actual native producers
with a real ROM tile and distinguishable branch palettes:

- Y-step 1 plus overlapping descriptors: first opaque texel and later
  descriptor ownership remain exact; visible pixels at 1/2/3/4x are 17/34/51/68.
- Mirrored X/Y steps 144/173: sampled zoom remains exact; visible pixels are
  53/174/367/671.
- Nominal Y=23, Y-step 16, ending exactly at Y=24: zero visible leakage at all
  four scales, preserving culling before the `+255` raster phase.

These branches passed 24 scale/border/mode runs (25,920 native frames, 72
boundary branches). All layer/composite mismatches are zero. Native/audio CRC,
cycles/blocks and state CRC within each geometry are identical across modes;
branch restore/replay remains exact. A further 1600-frame fit/both run changes
scale nine times including retained trail history, exercises all six induced
scenario types and restores exact snapshots/baseline CRC. Ending here is an
induced unsupported producer, **not** a played campaign ending.

Requested captures now include PF0–3, SP0–3 and text, not just PF layers.
Reproduce the boundary matrix with:

```sh
build/f3rt-gpu-regression --seed 5 --frames 1080 --every 1080 \
  --scale 4 --border 48 --interp fit --interp-fields geometry,palette \
  --layers --inject-frame 1080 --inject-sprite-boundaries \
  --capture-frame 1080 --dump-dir /tmp/f3-gpuvideo/sprites/repro
```

Use each scale 1–4, border 0/48 and mode off/linear/fit. Full commands, logs,
counts and PNGs are under `/tmp/f3-gpuvideo/sprites/{proof*,boundaries/*,
transitions.*,windows/*,bench*}`. No ROMs, snapshots, captures or proof programs
are committed.

### Sprite-scene frame times

Same final binary, border 48, frozen native scenes, 25 warmup plus 100 isolated
repeats. Entries are **mean / p95 / worst milliseconds**. GPU timing includes
submission, explicit fence and RGB readback; it is not pure GPU execution or
end-to-end frontend pacing. No additional sprite pass/work was introduced.

| Frame | Scale | CPU threaded reference | GPU off | GPU fit geometry |
| --- | ---: | --- | --- | --- |
| 1080, zoomed | 1 | 0.524 / 0.781 / 0.813 | 1.288 / 1.698 / 3.335 | 1.023 / 1.125 / 1.314 |
| 1080, zoomed | 2 | 1.954 / 2.959 / 3.005 | 1.164 / 2.372 / 6.866 | 1.234 / 1.307 / 1.341 |
| 1080, zoomed | 4 | 7.510 / 8.818 / 11.485 | 2.063 / 2.429 / 4.695 | 2.088 / 2.186 / 2.274 |
| 6000, unscaled | 1 | 0.509 / 0.778 / 0.798 | 0.750 / 1.078 / 1.341 | 0.525 / 0.576 / 0.592 |
| 6000, unscaled | 2 | 2.164 / 3.234 / 3.384 | 0.835 / 0.895 / 0.932 | 0.866 / 0.943 / 0.971 |
| 6000, unscaled | 4 | 7.394 / 8.870 / 10.093 | 1.981 / 2.188 / 2.267 | 2.038 / 2.098 / 2.191 |

Both modes leave sprites unchanged; timing differences are driver/CPU
variation, not a claimed new sprite optimization. Linear and separately enabled
palette measurements are in `sprites/bench-runs.json`. Canonical bytes remain
unchanged after every measurement, zero fallback instructions.

### Actual sprite frontend and unchanged native output

Foreground Cocoa/Metal off/linear/fit geometry runs each execute 1600 native
frames at 4x/border 48 through the real key-event frontend. Actual introduction
and character-select window surfaces were inspected alongside fenced isolated
sprite captures. Background/non-activated Cocoa screenshot attempts were not
reliably frame-aligned; they are not substituted for these foreground checks.
Every run retains native RGB CRC `90d70624`, 434,311,748 cycles, 23,892,754
native blocks, 807,846 audio frames and zero fallback instructions. Each native
compare checks 101,634,560 RGB pixels with zero mismatches; WAV files are
byte-identical across modes and the earlier diagnostic runs.

Synchronous OS screenshots are stalls, not ordinary frame work: each run has
three pacing resyncs. Off/linear queue mean/max are 40.426/56.248 and
32.989/43.950 ms with zero queue drops; fit is 25.522/66.328 ms with one queue
drop. These numbers do not replace the separate ordinary automatic-integer
pacing proof above or claim that captures are free. Foreground commands/
metrics and actual surfaces are in `sprites/windows/foreground-{plan,
results}.json`, `fit-geometry-foreground/run.json` and the three foreground
capture directories. Full-resolution source captures/metrics remain external;
throwaway executables and sources were removed after smoke proof.

Current integrated `f3rt-check` passes. The GPU-off Cocoa frontend also accepts
the field-control CLI without GPU support and presents its native boot surface.
No sprite/canonical data layout, CPU ABI, machine/audio semantics, rollback
schema or existing MAME acceptance path changed. Metal on this Mac is exercised;
other GPUs, another monitor and a played campaign ending remained unverified
at that checkpoint.

## One-quad inverse sprite raster: Linux cutover

The sprite-only cutover replaced 256 texel quads per descriptor with one
six-vertex quad covering their union. At 1024 sprites this reduces submitted
vertices from 1,572,864 to 6,144, without changing assets, scene words, uniforms,
shader resource slots, CPU rendering, snapshots, interpolation or frame pacing.

For one axis, let `p = descriptor_origin * S + phase`, `k = texel_step * S`,
and `origin` be the render-target offset in output pixels. Texel `n` starts at
`floor((p + n*k)/256) - origin`. The vertex shader supplies
`bias = 256*(origin+1) - p - 1`; at destination pixel `d`,
`U = 256*d + bias` identifies the last starting texel as `U/k` for `U >= 0`.
X spans tile exactly, so this skips collapsed zero-width columns. Y steps
`k >= 256` need one lookup. Smaller Y steps scan only rows starting on the
same output pixel, in logical order, until the first masked nonzero pen;
there are at most 16 candidates. Flips change the asset fetch, not precedence.
Nominal culling, constant X/Y phases 128/255, native-plane scissor and later
sprite ownership are unchanged. Native producers supply positive steps 1..256.

### Exercised correctness

Release Linux x86-64, SDL3 Vulkan, AMD Radeon RX 7800 XT / RADV Mesa 26.2.2:

- Independent forward-span/inverse-span checks covered 28,672 scale/step/phase
  combinations, including negative fractional origins; an additional 12,288
  Y combinations checked the uncrushed fast path.
- A throwaway independent forward-raster smoke passed before and after the
  cutover: 384 scenes, 18,432 descriptors, 836,001,792 exact RGB comparisons.
  Scales 1..8, borders 0/48, all flips, masks 15/63, transparent early rows,
  collapsed columns, descriptor overlap and all clipped edges were exercised.
- Eleven real-ROM seed-5 runs passed all-layer/composite comparisons and
  66 native-producer sprite boundary groups: all player scales 1..4 at borders
  0/48, diagnostic scale 8 at borders 48/160, and a live 1→2→4→8→1→3 run.
  Off/linear/fit and separately enabled palette interpolation were exercised.
  New permanent batches cover one-pixel Y thresholds, X collapse, flipped
  first-opaque selection, transparent show-through and edge clipping. The
  existing crushed overlap, mirrored zoom and nominal top-cull cases remain.
- The scale-4/border-48 frame-1560 independent CPU-backend check retained zero
  snapshot-byte and GPU-pixel differences. Before/after native frame CRC
  `a38b55e4`, audio CRC `4c7823c0`, state CRC `768cf94d`, cycles 423,453,931 and
  native blocks 23,418,681 matched; interpreter fallback instructions were zero.
- The actual Wayland frontend completed 1200 unthrottled frames at scale 3 /
  border 48; its captured GPU surface was inspected. Native frame CRC
  `e8cc7573`, cycles 325,733,798 and native blocks 19,123,943 matched the baseline.
- A paced 600-frame Wayland automatic-integer/fit-geometry run selected scale 4
  at 2496×1392 window pixels, completed in 10.91 seconds including startup,
  and reported zero clock resyncs / one audio queue drop. Queue mean/max were
  27.9792/51.1408 ms; this is not a zero-drop audio claim.

Both SPIR-V shaders passed `spirv-val --target-env vulkan1.0`; offline generation
also produced the MSL shaders. This cutover was **not runtime-tested on Metal**
or on the lower-end Linux hardware reporting slowdowns. Independent throwaway
sources/executables were removed after verification.

### Measured cost, not a general Linux fullspeed claim

The same frozen seed-5 frame 1560 at scale 4 / border 48, 100 repetitions after
five warmups, measured whole compositor submission/fence/readback latency:

| Sprite raster | Mean ms | p95 ms | Worst ms |
| --- | ---: | ---: | ---: |
| 256 texel quads per sprite | 1.14849 | 1.33146 | 1.38168 |
| One inverse-sampled quad per sprite | 0.987872 | 1.07653 | 1.17336 |

The observed mean reduction is 14%; these are separate runs, not hardware
GPU timestamps. These measurements precede the subsequent upstream
motion-interpolation merge; they are not new timings of that merged renderer.
Synthetic opaque 1024-sprite timings were essentially unchanged
(0.634→0.624 ms). Whole frontend startup-plus-1200-frame time was likewise about
7 seconds before and after; the after run additionally saved a final surface.
The desktop therefore does not establish excessive geometry as the sole cause
of friends' slowdowns, or demonstrate a material end-to-end speedup. Those sprite-only
measurements left VSync and the existing frame limiter unchanged; the later
presentation/audio pacing fix is recorded below.

Reproduce the real-ROM timing/boundary check with:

```sh
cmake --build build --target landmakr f3rt-gpu-regression -j 4
./build/f3rt-gpu-regression --seed 5 --frames 1560 --scale 4 --border 48 \
  --every 120 --layers --inject-sprite-boundaries --bench
```

For the compact parity cases use `--frames 1440 --every 720`, select the scale
and border, and retain `--layers --inject-sprite-boundaries`; default injection
frame 1407 is the exercised producer boundary. Diagnostic scale 8 remains outside
the player's scale-4 cap.

### Upstream motion-interpolation integration

After integrating upstream `main` at `a77d867`, both ordinary and temporal draws
use the same six-vertex sprite pass. The merged Release build passed:

- Scale-3/border-48/fit/both-fields seed-5 parity through 1440 frames, including
  all isolated layers and all six native-producer boundary groups: zero mismatches.
- `f3rt-motion-regression --seed 5 --frames 1600 --scale 4 --every 20 --interp fit`:
  81 sampled frames, 20 visible ROM midpoints, 1206 accepted sprite geometry
  draws, exact current-frame endpoints/repeated draws/native pixels/audio/state,
  and discontinuity snapping plus replay. Half-pixel text sampling was exact.
- CTest `motion-interpolation-guards`.
- A real 1500-frame Wayland frontend with `--motion-interp`, scale 3 / border 48
  and fit geometry: 6078 drawable submissions, 786 interpolated submissions,
  zero interpreter fallback instructions. The captured player-select surface
  was inspected. Submission counters are not physical scanout measurements.

## Linux presentation backpressure and audio

Sprite geometry was not the only bottleneck. Ordinary GPU presentation used
FIFO/VSync and `SDL_WaitAndAcquireGPUSwapchainTexture` on the same thread that
advances native simulation and produces PCM. A slow display could therefore
throttle emulation even when the shaders finished well within a native frame.
Zero `audio_queue_drops` means the backlog cap never cleared the stream;
it does not rule out audio starvation.
`clock_resyncs` counts native-clock lateness beyond the existing 50ms window.

Presentation now prefers tear-free **mailbox** when supported; unthrottled mode
still prefers immediate presentation. FIFO remains the compatibility fallback.
The paced frontend uses `GpuVideo::present` / `present_motion`: acquire before
uploads, skip a busy in-flight frame, and cancel its empty command buffer.
Rejected draws do not cycle upload/render resources, enqueue GPU work, or change
native simulation, audio generation, scene/history capture or snapshots.
Explicit screenshots/final surfaces and offscreen diagnostics still render and
wait; they cannot silently capture an older surface after a skipped draw.

Without a native display pacer, motion mode schedules display submissions against
a rolling display-mode deadline and sleeps until the earlier display/native
simulation deadline. This preserves native-rate PCM on a slower display without
busy-spinning on a rejected drawable. Paused motion menus also observe the
display deadline rather than rendering on every UI poll. Display changes refresh
the interval.
The standalone frozen motion-comparison demo uses the same deadline fallback;
macOS retains its native display-link pacing.

`SDL_AcquireGPUSwapchainTexture` alone is insufficient to remove display pacing:
[SDL 3.4.16's Vulkan implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/gpu/vulkan/SDL_gpu_vulkan.c#L10210-L10246)
checks the oldest in-flight GPU fence without waiting, but subsequently calls
`vkAcquireNextImageKHR` with an unlimited timeout. Mailbox avoids FIFO display
backlog in addition to skipping busy GPU frames. Drivers without mailbox support,
other driver/WSI stalls, and the friends' lower-end hardware remain unverified.
This is not a universal nonblocking-driver or 60fps-rendering guarantee.

### Controlled live reproduction

Release Linux x86-64, SDL 3.4.16 Vulkan, RX 7800 XT / RADV Mesa 26.2.2,
Gamescope 3.16.28 nested on GNOME Wayland. A 30Hz nested compositor reproduced
lateness at **scale 1**, without an artificial delay or heavier sprite shader.
A 600-frame CPU control had zero clock resyncs; the GPU run had 103, with zero
queue drops. Both retained the same native frame CRC/cycles/audio sample count.

Matched 1200-frame GPU runs used a temporary SDL interposer only to time actual
acquisition/audio calls and mute host output; it did not delay calls or fake
backpressure. Generated PCM remained nonzero and byte-identical before/after.

| Metric | Before | After |
| --- | ---: | ---: |
| Clock resyncs | 176 | 0 |
| Audio queue drops | 0 | 0 |
| Post-enqueue queue mean / maximum (ms) | 23.9338 / 51.1408 | 28.1283 / 41.0940 |
| PCM enqueue interval mean / maximum (ms), after 20-frame warmup | 26.9091 / 65.8424 | 16.9691 / 22.9815 |
| PCM enqueue intervals above 25ms, after warmup | 471 | 0 |

All 1200 native frames retained CRC `e8cc7573`, 325,733,798 CPU cycles,
19,123,943 native blocks, zero interpreter fallback instructions, 605,885 audio
frames, peak 158 and 131,670 nonzero samples. WAVs were byte-identical. Final
320x232 GPU captures were visually inspected and every RGBA pixel matched.
Timing samples above precede the explicit final capture; submission/acquisition
measurements are not scanout or an auditory listening claim. Nested Wayland
teardown warnings occurred in both versions and were not a shutdown validation.

Reproduce the display-backpressure scenario (muted playback):

```sh
gamescope --backend wayland --expose-wayland -r 30 -o 30 \
  -w 960 -h 696 -W 960 -H 696 -- \
  env SDL_VIDEODRIVER=wayland ./build/landmakr \
  --config /tmp/f3-pacing.conf --video game --video-backend gpu \
  --video-scale 1 --video-border 0 --video-interp off \
  --postprocess off --volume 0 --frames 1200
```

Repeat with `--video-backend cpu` for the control, or add `--motion-interp`
to exercise the independent display/native deadlines.

Additional live checks on the same GPU:

- 1200-frame motion mode at 30Hz: 612 drawable submissions in 20.361s while
  native simulation remained about 58.94Hz; zero resyncs/drops/history resets.
  PCM enqueue mean/max 16.9691/20.7001ms, zero intervals above 25ms after warmup;
  queue mean/max 27.8063/41.2957ms. WAV bytes and native CRC/cycles/blocks matched
  ordinary mode. Submission counts are not physical scanout.
- A temporary real SPIR-V postprocess workload at scale 3 / border 48 exercised
  two busy-acquisition rejections, without faking SDL results. All 1200 native
  frames and WAV bytes remained exact, with zero clock resyncs; PCM enqueue
  mean/max 16.9691/20.4616ms and zero intervals above 25ms after warmup. One
  backlog drop occurred, queue mean/max 29.9934/66.3284ms. The final capture
  explicitly waited and completed. This exercises rejection/capture behavior,
  not sustained underpowered-GPU performance.
- Motion mode with the same real postprocess workload at reported 240Hz:
  2701 of 3909 acquisition attempts returned no drawable, while 1200 native
  frames completed in 20.3851s with zero resyncs/history resets. PCM enqueue
  mean/max 16.9690/20.5992ms, zero intervals above 25ms after warmup; one backlog
  drop, queue mean/max 28.1195/51.1408ms. Native counters and WAV bytes matched
  ordinary mode. The forced final alpha-1 capture matched every RGBA pixel of
  the ordinary-workload 1248x696 capture, despite the rejected temporal draws.
- Actual X11 motion/fit/auto-integer UI: F1 pause/resume, resize and F11 enter/leave;
  scales 3→1→3→2, 1200 native frames, exact native counters and WAV bytes. Menu
  redraws stayed at display cadence after applying its deadline guard; captured
  menu/final game surfaces were inspected. This interaction/capture run recorded
  one clock resync and zero queue drops (27.636/41.7997ms mean/max). Its enqueue
  intervals include the intentional 1.595s pause and are not steady-state audio
  performance evidence. Gamescope's WSI bypass was disabled for X11 window
  capture; unsupported Wayland screencopy was not used as visual proof.
- Existing scale-3/border-48/fit/both-fields GPU parity through frame 1440, with
  all isolated layers and native sprite boundaries, passed with zero mismatches.
  Scale-4 fit motion regression through 1600 frames passed 81 samples, 70 pairs,
  20 visible intermediate frames, exact endpoints/repeats/native audio/state,
  discontinuity snapping and replay. CTest `motion-interpolation-guards` passed.
- Live frozen comparison on the reported 240Hz Wayland display mode: 235 native
  render-only pan steps and 939 drawable submissions in four seconds, including
  937 interpolated submissions; frozen native/state/sync bytes remained exact.
  The full-window left-native/right-motion capture was inspected. The measured
  first reporting window was 229.8 submissions/s; this is not a 240Hz scanout
  claim or fullspeed proof for lower-end GPUs.

Temporary instrumentation/workload/interaction sources are not permanent tests;
the reproduction depends on real GPU/display/audio services and is not an
isolated, deterministic full-suite-safe regression.
