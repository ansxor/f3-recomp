# Text layer: GameText

**What you will learn:** how `GameText` snapshots the raw text map and glyph RAM
from video RAM at VBSTART, and how its optional debug write log reports
unmodeled writers.

The files are `runtime/renderer/game/text.hpp` and `runtime/renderer/game/text.cpp` (including the
shared `decode`). The Land Maker store-PC list is in
`games/landmakrj/video/video.cpp`.

## What the class holds

The text layer is a raw VBSTART snapshot, not a decoded texture. Emulation keeps
writing VRAM while a frame is composited or materialized lazily, so the bytes
below the frame are captured at VBSTART and decoded on demand.

The game draws text with 8x8 glyphs. A map of 64x64 cells (512x512 pixels) names
the glyph of each cell. The glyph pixel data lives in character RAM.

| Member | Type | Meaning |
| --- | --- | --- |
| `map_` | `std::array<uint16_t, 4096>` | The raw big-endian text-map word of each cell. |
| `glyph_ram_` | `std::array<uint8_t, 0x2000>` | The raw 256 glyphs x 32 bytes of character RAM. |

## Raw bit layout

The text-map word (graphics offset `0x1c000`, one 16-bit word per cell):

- bits 0-7: glyph number (0 to 255);
- bit 8: flip X;
- bits 9-14: palette code (palette base = code * 16);
- bit 15: flip Y.

Character RAM (graphics offset `0x1e000`, 256 glyphs of 32 bytes, 8x8 4bpp):
pixel `(x, y)` of a glyph is the nibble `(x & 1)` of byte `y * 4 + (3 - x / 2)`,
matching `decode_charram_tile` (`runtime/renderer/decode.hpp`) and the FDP renderer.

The same layout is decoded in `runtime/renderer/shaders/scene_body.glsl` from the GPU
`TEXT_CELLS`/`GLYPHS` regions (see `runtime/renderer/shaders/scene.glsl`).

## Decode

At VBSTART `decode(vram)` copies:

- the text map at graphics offset `0x1c000`: 4096 big-endian words;
- character RAM at graphics offset `0x1e000`: `0x2000` bytes.

Both are read directly, so no ownership or completeness state is needed: the
decoder always produces a complete texture from the current bytes.

## Write log

With `--discovery-log`, stores to `0x61c000..0x61ffff` (map and glyph RAM) are the
`text` layer. If `pc` is one of the known
store PCs in `text_covered_write` (`games/landmakrj/video/video.cpp`, via `video_writer_known`), nothing is logged. Otherwise a
`NEW video-write layer=text` line is written. The store still
happens, and the next VBSTART snapshots it; no state is invalidated. See
[Video write logging](/developer/runtime/video/producers).

## Pixel sampling

`pixel(x, y, flipped)` decodes the raw words on demand:

1. `x &= 511` and `y &= 511`. If flipped, `x = 511 - x` and `y = 511 - y`.
2. The cell word is `map_[(y / 8) * 64 + x / 8]`.
3. The pixel position inside the glyph is `(x & 7)` and `(y & 7)`, each XORed
   with 7 for a flip (bit 8 for X, bit 15 for Y).
4. The pen is nibble `(tx & 1)` of glyph byte `(word & 0xff) * 32 + ty * 4 +
   (3 - tx / 2)`.
5. The result is `palette = ((word >> 9) & 0x3f) * 16 + pen` and
   `flags = pen ? 0x10 : 0`.

The compositor calls it with `row.text_x` and `row.text_y`, which
`GameLines::prepare` computes from the pivot scroll words.

## Saved state

Like the playfield tiles, text is rebuilt from the serialized video RAM at the
next VBSTART, so the raw snapshot is not part of canonical `GameVideo` state.
`load_state` restores the already-composited `pixels` and clears the pending
native frame, and `decode()` runs at VBSTART before anything reads `GameText`.
`GameVideo` copies the decoded `GameText` into the `CapturedFrame` at VBSTART; the CPU
reference path reads that copy, and `encode()` (`runtime/renderer/gpu/encode.cpp`) packs it
into the `TEXT_CELLS` / `GLYPHS` words of the GPU upload.

## Invariants

- `decode` must use the same nibble order as the FDP renderer, or text parity
  breaks. The CPU (`pixel`) and GPU (`scene_body.glsl`) decoders must agree.
- The layer has no bitmap mode. A frame with `bitmap` set in any row falls back
  (`bitmap-pivot`; see [GameVideo](/developer/runtime/video/game-hle)).

## Unit check

`check_game_text_vram_decode` in
[runtime/check.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/check.cpp)
builds raw text-map words and glyph bytes and checks the palette base, pen,
flip bits and glyph nibble order. The text-layer comparison of the gameplay
regression (layer mask 256) is the full proof. See
[Parity evidence and limits](/developer/runtime/video/parity).

Sources: [text.hpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/renderer/game/text.hpp),
[text.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/renderer/game/text.cpp) and
[games/landmakrj/video/video.cpp](https://github.com/ansxor/f3-recomp/blob/main/games/landmakrj/video/video.cpp).
