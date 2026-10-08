# Land Maker game-data video

Target: supplied **Japan 2.01J (`landmakrj`)** program. Addresses below are for
that program, not the World revision. Generated program C and ROM/asset bytes
remain untracked.

## Video RAM is the source of truth

The game scene is no longer reconstructed by observing game code and work RAM
through recompiler hooks. At every **VBSTART**, `GameVideo` builds a
[`VideoRam`](../../../runtime/renderer/game/scene.hpp) view of the FDP video RAM that the
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

`runtime/renderer/fdp/video.cpp` remains the independent **MAME-derived FDP reference**
(`inspect_scene_row`, `inspect_playfield_line`, `sprite_plane`, …). Its output is
a software compatibility target, not verified physical TC0630FDP behavior. The
game decoders read the same video RAM through shared decode primitives
(`Video::decode_charram` and `Video::get_sprite_info` call
`decode_charram_tile` / `decode_sprite_list`). The line-RAM walk in
`runtime/renderer/game/lines.cpp` is shared by all games, but remains a separate copy of
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

## Compile-time geometry and per-game features

The scene stack takes its scanout window from `[video]` in `games/<game>/config.toml` through
`f3rt::geometry` (`include/f3rt/game_video.hpp`, backed by the generated
`generated_config/game_video_config.hpp`): `first_line`, `height`, `end_line`. Land Maker is
24/232, Command War 32/224. The GPU shaders receive the same values as a uniform. `GameVideo`
throws at construction when the loaded `RomSet::video` does not match
(`game_config::matches`). The CTest `game-geometry-literals` rejects a bare `24`/`232` in the
renderer, GPU and shader sources. The horizontal window (46..365) is the same on every game.

Features beyond the base Land Maker layout, all mirrored from the FDP:

- `extended_alt_maps`: PF2/PF3 alternate maps 4/5 (`0x18000`/`0x1a000`), chosen per scanline by
  line RAM bit `0x200` (`ScenePlayfield::alt_map`). The GPU stores the alternate cell in word 1
  of the PF2/PF3 cell slots (`F3_LAYER_ALT_MAP`). `--video compare` also compares both maps.
- `full_resolution_alt_maps` (needs `extended_alt_maps`; render-only, default off): Command War
  uploads each floor twice, a full-resolution copy into the main map and a horizontally half-scaled
  copy (X2/Y1) into the alternate map, and draws distant floor rows from the half copy because
  `x_step` cannot exceed 256. `FullResolutionAltMaps` (`tiles.cpp`) solves, per PF2/PF3 tile row,
  the offsets `c` in `[0, 1024)` with `alt(x, y) == main((2x + c) mod 1024, y)` for every x and all
  16 lines. It compares the final `RowSampler::pixel` sample (palette, pen, blend flag), not raw
  cells, skips rows with no tile code, and caches per row keyed on both maps' raw cells (only
  uploaded rows are solved again; ~1.4 us per frame average, 0.75 ms worst frame over 6000
  attract frames). `GameLines::resolve_full_resolution_alt` stores the smallest `c` valid for every
  tile row a scanline can sample in `ScenePlayfield::full_res_offset` (-1: fall back). The canonical
  row fields never change. `presented_playfield()` (`scene.hpp`) is the single remap: `alt_map=false`,
  `x_step*2`, `source_x = 2*source_x + (c << 8)` wrapped to 1024 px; `y` is unchanged. It is
  applied only to expanded output: the CPU compositor (`FrameScene::presented`, expanded kernels)
  and `gpu/encode.cpp` (`options.expanded()`), so the native frame, `--video compare`, snapshots,
  `state_crc` and `sync_state_crc` are untouched. A motion-interpolated row keeps moving
  horizontally through the same remap; one whose vertical phase moved keeps the alternate map.
  The shader and compositor already handle `x_step` up to 512. Rows with no exact twin (the
  half copy is not 512-periodic, or its tiles are not whole-map aliases) keep canonical alternate
  sampling; `GameVideo::report` appends `presented_alt_rows_remapped/fallback/solved` to the
  `VIDEO presented_sprite_frames` line (visible, enabled, non-culled alt rows over rendered frames).
  Measured on Command War attract, 6000 frames: 373,575 alt rows remapped, 132,764 fallback (26%).
  Of the 640 row solves, 142 were not 512-periodic, 96 had no tile code and 402 matched
  (typically `c` = 32, 288, 544 or 800; period 256). For the unmatched floor rows (PF3 rows 12..18)
  98% of the best-offset mismatches are visible pixels that differ in palette bank and tile art,
  not transparent-pixel or colour-equivalent differences, and no row shift helps.
- Row-usage cull: a map row with no nonzero tile code is not drawn (`ScenePlayfield::empty_row`).
- Asset ROM wrap: PF codes are 16-bit and sprite codes 17-bit, wrapped to the ROM tile count
  (`wrap_tile_index`; the GPU receives both counts in the uniform).
- Per-line 15-bit palette words and horizontal blur (line RAM `0x6000` section word 2, bits
  14/13): CPU compositor and GPU shader. The interpolation analysis refuses 15-bit rows.

## Per-game scene code

Each game opts into the de-HLE'd scene by providing a `games/<game>/video/`
folder. `landmakrj` and `commandw` each provide a single file, `games/<game>/video/video.cpp`,
that defines only the debug hook below. `GameTiles::decode`, `GameText::decode` and
`GameSprites::decode` are shared in `runtime/renderer/game/tiles.cpp`,
`text.cpp` and `sprites.cpp`:

| Symbol | Purpose |
| --- | --- |
| `observe_game_video_write` | Debug store-PC check for every component |

The runtime keeps the generic parts: `runtime/renderer/game/lines.cpp` defines the
shared `GameLines::decode`, `runtime/renderer/decode.{hpp,cpp}` holds the shared
char-RAM tile unpack and the sprite display-list walk, and the component
classes hold sampling (`GameTiles::RowSampler`, `GameText::pixel`,
`GameSprites::raster`, `GameLines::prepare`) and state serialization. The debug
`observe_game_video_write` entry point exists only when `F3RT_VIDEO_WRITE_LOG` is enabled
(see [Unsupported stores and fallbacks](#unsupported-stores-and-fallbacks)). CMake
compiles `games/${F3_GAME}/video/*.cpp` and defines `F3RT_GAME_VIDEO`; games
without the folder still link (`runtime/renderer/game/video_generic.cpp`) but cannot
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
`runtime/renderer/game/lines.cpp`, a shared decoder that mirrors `Video::read_line_ram`:
the `$4000`..`$b000` sections, subsection latch carry-forward, clip planes,
blend/alpha, mosaic, pivot and sprite mixing, PF zoom/palette-add/rowscroll/mix.

## Unsupported stores and fallbacks

Store logging is opt-in. Only a build configured with `F3RT_VIDEO_WRITE_LOG`
(`OFF` by default; defined publicly on `f3rt`) makes `Machine::write8` call
`GameVideo::observe_write(pc, address)` for graphics and control writes. It
calls the per-game `observe_game_video_write(pc, address, frame)`, which picks
the component by address, returns for the store PCs that component knows and
otherwise calls `log_unknown_video_write(layer, pc, address, frame)`, which prints the
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
