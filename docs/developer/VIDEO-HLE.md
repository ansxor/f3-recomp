# Land Maker game-data video

Target: supplied **Japan 2.01J (`landmakrj`)** program. Addresses below are for
that program, not the World revision. Generated program C and ROM/asset bytes
remain untracked.

## Video RAM is the source of truth

The game scene is no longer reconstructed by observing game code and work RAM
through recompiler hooks. At every **VBSTART**, `GameVideo` builds a
[`VideoRam`](../../../runtime/game_scene.hpp) view of the FDP video RAM that the
standalone renderer already reads:

- `graphics` — 0x40000 bytes at `$600000` (sprites `$00000`, playfield maps
  `$10000`, text map `$1c000`, character/glyph RAM `$1e000`, line RAM `$20000`,
  pivot RAM `$30000`);
- `control` — 0x20 bytes at `$660000` (PF0..PF3 X/Y scroll, then pivot/text
  registers).

and calls the four scene decoders before rendering:

```cpp
const VideoRam vram{m.graphics, m.control, m.frame + 1};
tiles.decode(vram);     // raw playfield cells
text.decode(vram);      // text map + glyph RAM
sprites.decode(vram);   // hardware sprite display list
lines.decode(vram);     // 256 per-scanline line parameters
render();               // compose from the previously latched sprite plane
```

`runtime/video.cpp` remains the independent **MAME-derived FDP reference**
(`inspect_scene_row`, `inspect_playfield_line`, `sprite_plane`, …). Its output is
a software compatibility target, not verified physical TC0630FDP behavior. The
game decoders read the same video RAM through shared decode primitives
(`Video::decode_charram` and `Video::get_sprite_info` call
`decode_charram_tile` / `decode_sprite_list`). The line-RAM walk in
`games/landmakrj/video/lines.cpp` is still a separate copy of
`Video::read_line_ram`.

The one-frame FDP sprite lag is preserved: `render()` composes using the
previously latched sprite plane, and `GameVideo::latch_sprites()` latches the
newly decoded submission afterwards, exactly as `Video::vblank` buffers the old
list. Immutable decoded ROM textures are shared to avoid a duplicate 16 MiB
asset decode.

Color indices resolve through the shared FDA palette RAM. The game compositor
takes raw playfield cells (sampled on demand), decoded glyph pens, sprite
geometry and per-row descriptions. Strict-native `landmakr` defaults to `game`;
`fdp` is the diagnostic/fallback-execution default. `compare` renders both and
rejects any supported-frame difference.

## Per-game scene code

Each game opts into the de-HLE'd scene by providing a `games/<game>/video/`
folder. `landmakrj` provides one file per component:

| File | Defines |
| --- | --- |
| `games/landmakrj/video/tiles.cpp` | `GameTiles::decode` (raw 4-byte cells) / `observe_write` |
| `games/landmakrj/video/text.cpp` | `GameText::decode` (map + glyph RAM) / `observe_write` |
| `games/landmakrj/video/sprites.cpp` | `GameSprites::decode` (display list) / `observe_write` |
| `games/landmakrj/video/lines.cpp` | `GameLines::decode` (line RAM + control) / `observe_write` |

The runtime keeps the generic parts: `runtime/video_decode.{hpp,cpp}` holds the
shared char-RAM tile unpack and the sprite display-list walk, and the component
classes hold sampling (`GameTiles::RowSampler`, `GameText::pixel`,
`GameSprites::raster`, `GameLines::prepare`) and state serialization. The debug
`observe_write` entry points exist only when `F3RT_VIDEO_WRITE_LOG` is enabled
(see [Unsupported stores and fallbacks](#unsupported-stores-and-fallbacks)). CMake
compiles `games/${F3_GAME}/video/*.cpp` and defines `F3RT_GAME_VIDEO`; games
without the folder still link (`runtime/game_video_generic.cpp`) but cannot
select `--video game|compare`.

## Decoded layouts

**Playfield cells** are the raw 4-byte big-endian map entries. The GPU scene
buffer copies them verbatim and the shader decodes them exactly like
`GameTiles::RowSampler`; the CPU compositor decodes the same words for non-GPU
builds, captures and reference paths.

| Attribute bits | Meaning |
| --- | --- |
| `0..8` | Palette row; base index = row × 16 |
| `9` | Blend selector |
| `10..11` | Extra-plane selection; pen mask = `((planes & ~palette_row) << 4) | 15` |
| `14` | Horizontal flip |
| `15` | Vertical flip |

**Text** is the map word `tile | palette<<9 | flip_x<<8 | flip_y<<15` per cell
plus the 8×8 4bpp glyph RAM, decoded with the same nibble order as
`Video::decode_charram`.

**Sprites** are parsed from the hardware display list with the shared
`decode_sprite_list`, also called by `Video::get_sprite_info`: block chaining,
the list-jump word, command/zoom/scroll words and the sprite-bank select. Command word 5 carries screen flip (bit 13),
extra pen planes (bits 8–9), trails (bit 1) and bank (bit 0). Positions, zoom and
flips come from video RAM, not from work-RAM scroll mirrors.

**Lines** decode all 256 scanlines from line RAM + control registers in
`games/landmakrj/video/lines.cpp`, a separate copy of `Video::read_line_ram`:
the `$4000`..`$b000` sections, subsection latch carry-forward, clip planes,
blend/alpha, mosaic, pivot and sprite mixing, PF zoom/palette-add/rowscroll/mix.

## Unsupported stores and fallbacks

Store logging is opt-in. Only a build configured with `F3RT_VIDEO_WRITE_LOG`
(`OFF` by default; defined publicly on `f3rt`) makes `Machine::write8` call
`GameVideo::observe_write(pc, address)` for graphics and control writes. Each
component's `observe_write` returns for the store PCs it knows and otherwise
calls `log_unknown_video_write(layer, pc, address, frame)`, which prints the
first occurrence of each `(layer, pc)` to stderr so unmodeled routines can be
collected for future implementation. With the option `OFF` there is no write
observation at all and the graphics path keeps its fast `direct_bytes` copy. A
store from an unknown PC never invalidates anything in either build: the data
still comes from video RAM.

Three genuine features remain outside the measured normal-orientation contract
and force the FDP oracle for that frame. `Impl::fallback(component, reason)`
always calls `log_unsupported_video(component, kind, frame)`, which prints the
first occurrence of each `(component, kind)`:

| Fallback | Trigger | Logged component / kind |
| --- | --- | --- |
| `flipped-screen` | sprite command bit 13 | `sprites` / `flipped-screen` |
| `sprite-trails` | sprite command bit 1 | `sprites` / `sprite-trails` |
| `bitmap-pivot` | pivot control `$a0` bits set on a visible row | `text` / `bitmap-pivot` |

The report names the fallback reason, count and first/last affected frame
(`VIDEO fallback=<reason> frames=N first=F last=L`); it is not CPU interpreter
fallback. The default oracle path remains available for every frame.

## Snapshot state

Scene data is derived from video RAM (already serialized as part of `Machine`),
so the canonical GameVideo snapshot dropped the playfield tile payload and all
hook-only bookkeeping (tile validity/unsupported PCs, text references and glyph
completeness, sprite support/scroll mirrors, line control mirrors). Playfield
tiles and text are both rebuilt from the serialized video RAM by `decode` at the
next VBSTART and are not saved; `load_state` restores the composited pixels.
`GameLines` still serializes decoded line params + rows; sprite
staging/submitted/current lists and the command flags remain. See
[ABI-CHANGES.md](ABI-CHANGES.md).

## Opt-in presentation

| Option | Default | Operation |
| --- | --- | --- |
| `--video-scale 1..4\|auto\|auto-integer` | `1` | Rerasterizes scene geometry at the requested internal resolution. GPU auto modes follow physical window pixels, clamped to 1–4. PF fractional sampling and sprite zoom are evaluated at the higher resolution; original ROM textures/glyphs remain the artwork. |
| `--video-border 0..160` | `0` | Adds that many native scene columns on each side. `48` gives a 416×232 viewport. |
| `--video-filter nearest\|linear` | `nearest` | Optional SDL presentation-texture filtering. |

Options require `--video game` or `compare`, which require `F3RT_GAME_VIDEO`.
The invariant native `Machine::native_pixels()` stays 320×232 for comparison,
captures and CRCs. Fallback frames preserve the exact oracle picture,
integer-scaled in the center, with black added columns.

## Acceptance

The scoped phase is complete: VRAM-owned scene reconstruction at VBSTART,
per-game `games/<game>/video/` decoders, opt-in PC/address-only write logging
(`F3RT_VIDEO_WRITE_LOG`), explicit `flipped-screen` / `sprite-trails` /
`bitmap-pivot` fallbacks and the opt-in presentation options are implemented.
`f3rt-gameplay-regression --frames 6000 --video-diff --video-diff-every 60`
reports pixel_mismatches=0 for all nine layers and the composite over 91 sampled
frames, with `VIDEO game_frames=6000 oracle_fallback_frames=0` and the
pre-change frame CRC.
