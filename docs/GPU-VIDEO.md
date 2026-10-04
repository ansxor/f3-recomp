# GPU presentation compositor

## Scope and checkpoints

Presentation only. Native `Machine::pixels`, strict-native execution, captures,
frame/replay CRCs, audio and rollback snapshots remain CPU-produced. FDP oracle
fallback remains available. The CPU path is retained and row-parallelized before
the GPU port. Checkpoints are distinct commits/tags:
`gpuvideo-1-threaded-cpu`, `gpuvideo-2-gpu-parity`, `gpuvideo-3-interp`.
Interpolation is not part of the parity contract or parity checkpoint.

## CPU sampling contract (read before shader implementation)

Sources: `runtime/game_compositor.cpp`, `game_tiles.cpp`, `game_lines.cpp`,
`game_sprites.cpp`, `game_text.cpp` and [VIDEO-HLE.md](VIDEO-HLE.md).
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
texel rows win when forced one-row spans overlap. GPU texel quads therefore
draw sprites in increasing list order, texels in reverse order, discard
transparent texels, and overwrite an integer indexed render target. This
reproduces both precedence rules without atomics or per-fragment list scans.

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

Immutable packed-byte PF and sprite graphics are uploaded once. Each frame
uploads semantic PF cells, text cells/decoded glyph pens, RGB palette,
normalized row tables (stable priority order and exact clip ranges), and the
**rendered** sprite list/pen mask. Semantic data avoids a second FDP-RAM reader.
A small uniform contains scale/border/dimensions and diagnostic layer selection.
GPU resources/transfer buffers are reused and cycled to protect in-flight data.

Pass 1 draws integer-aligned sprite texel quads into an indexed texture.
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

Seed 5 through 3600 frames additionally induced actual bitmap, trails, flip,
unknown-writer and ending-producer fallback boundaries at frame 3404, restoring
and replaying the original native frame after each branch. All five fallback
images and supported recoveries match CPU output; canonical restore is exact.
The ending case is an **induced producer boundary, not a played-through ending**.
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

`game_lines.cpp` already reconstructs the actual `0x9d66a` palette table,
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

The 4000-frame fit run also exercises induced bitmap, trails, flip, unknown
writer and ending-producer fallback/recovery, keeping canonical bytes exact;
ending remains an **induced boundary, not a played-through ending**.
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
