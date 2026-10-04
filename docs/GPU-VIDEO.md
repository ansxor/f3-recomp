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
