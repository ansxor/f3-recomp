# Playfield tiles: GameTiles

**What you will learn:** how `GameTiles` snapshots the four playfield maps from
FDP video RAM at VBSTART, the raw cell bit layout, how unknown writer PCs are
reported, and how a pixel is sampled on both the CPU and the GPU.

The class and its shared `decode` are in `runtime/renderer/game/tiles.hpp` and
`runtime/renderer/game/tiles.cpp`. The Land Maker store-PC list used by
`video_writer_known` (`--discovery-log`) is in `games/landmakrj/video/video.cpp`, linked only
for the configured `F3_GAME`.

## What the class holds

`GameTiles` stores four maps of 2048 raw `uint32_t` cells each (64 columns x 32
rows of 16x16 texels). Unlike the retired hook model, each cell is the **raw**
4-byte video RAM entry, not a decoded `Cell`, because emulation keeps writing
video RAM while a frame is composited or materialized lazily. The snapshot is
taken once per frame.

| Member | Meaning |
| --- | --- |
| `maps_` | `std::array<std::array<uint32_t, 2048>, 4>` of raw cells |
| `RowSampler` | per-row decode cursor over one layer |

## Raw cell bit layout

A cell is 4 big-endian bytes at graphics offset `0x10000 + layer * 0x2000`. The
snapshot folds them into one host word, `attributes << 16 | code`.

| Bits | Meaning |
| --- | --- |
| 0-8 | palette code; palette base is `code * 16` |
| 9 | blend selector |
| 10-11 | extra pen planes |
| 14 | flip X |
| 15 | flip Y |
| code (low 16 bits of the word) | tile number; low 15 bits index a 256-byte tile |

The pen mask is `(((attributes >> 10) & 3 & ~attributes) << 4) | 15`. The
`& ~attributes` uses the whole attribute, so an extra plane is masked off when
the matching low bit of the palette row is set (bits 0 and 1). This is the same
rule as `Video::generate_playfield_line` and the GPU shader (`scene_body.glsl`).

## Public functions

| Function | What it does |
| --- | --- |
| `reset()` | Clears all maps (called at machine reset before the first decode). |
| `decode(vram)` | Copies the four raw layers from video RAM. Shared, defined in `runtime/renderer/game/tiles.cpp`. |
| `playfield_pixel(layer, x, y, flipped, tiles)` | Returns a `ScenePixel` for texture position (x, y). |

There is **no snapshot state**. The maps are derived from the serialized video
RAM and rebuilt at VBSTART, so they are not part of `GameVideo` canonical state.

## Decode

`decode` copies layer `L` from graphics bytes `0x10000 + L * 0x2000`, one
4-byte cell per 16x16 tile, for all four layers. Video RAM is the source of
truth.

With `--discovery-log`, `video_writer_known` receives only the layer and store PC. Land Maker's tile-block copy/erase loops and per-layer clear
stores are accepted:

```text
0x55fc, 0x5646, 0x56d6,
0x5a2e, 0x5a6a, 0x5aa6, 0x5b00,
0x9bd08, 0x9bd0a, 0x9bd0c, 0x9bd0e,
0x9ec66, 0x9ec6a, 0x9ec6e, 0x9ec72
```

A write from any other PC in `0x610000..0x617fff` is **not** an error any more:
the snapshot is read back from video RAM regardless, so it cannot fake or lose a
layer. Under `--discovery-log` the PC is logged once (`video-write`, layer `pf0`..`pf3`), so unmodeled producers can be
collected for future work. Without the flag there is no write observation at
all.

## Pixel sampling

The CPU and the GPU decode the identical raw words.

`GameTiles::RowSampler::pixel(x)`:

1. Wrap: `x &= 1023` and `y &= 511`.
2. If the screen is flipped, use `x = 1023 - x` and `y = 511 - y`.
3. The cell is `maps_[layer][(y / 16) * 64 + x / 16]`; it is decoded when the
   column changes.
4. The texel position inside the tile is `(x & 15)` and `(y & 15)`, each XORed
   with 15 for a flip.
5. The pen is `tiles[(code & 0x7fff) * 256 + ty * 16 + tx] & pen_mask`. The
   `tiles` span is `Video::playfield_tiles()`.
6. Return `palette = palette_base + pen` and `flags = (pen ? 0x10 : 0) | blend`.

`GameVideo` copies the decoded `GameTiles` verbatim into the `CapturedFrame`
(`runtime/renderer/game/captured_frame.hpp`) at VBSTART. The CPU reference
renderer reads that copy directly; `encode()` in `runtime/renderer/gpu/encode.cpp`
writes the raw cells into the `F3_SCENE_PF_CELLS` region (one word per cell in the
two-word slot, layout in `scene_layout.h`) of the GPU upload, and `scene_body.glsl`
decodes `attributes`, `code`, flips, pen mask and blend with the same formulas.
`--renderer compare-cpu` still compares video-RAM decode against the FDP renderer.

## Unit check

`check_game_tile_observation` and `check_game_tile_row_sampling` in
[`runtime/tests/video.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/video.cpp) write raw cells into a synthetic graphics buffer, call
`decode`, and verify:

- Palette base, pen mask and blend selector decode.
- Flip X and flip Y mirror the sampled texel.
- The extra pen plane is masked off when the palette's low bit is set.
- Wrapping, repeats, reverse X jumps and global screen flip sample the correct
  cell across all four layers.

## Invariants

- Video RAM is the source of truth; decode it at VBSTART, never from work RAM.
- The pen-mask rule `& ~attributes` uses the whole attribute.
- The per-game `decode` snapshot must remain a copy; it may not alias live VRAM.
- A new tile producer needs only its store PCs added to `tiles_covered_write`;
  the decode itself needs no change because it reads video RAM.

Sources: [tiles.hpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/renderer/game/tiles.hpp),
[tiles.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/renderer/game/tiles.cpp) and
[video.cpp](https://github.com/ansxor/f3-recomp/blob/main/games/landmakrj/video/video.cpp).
