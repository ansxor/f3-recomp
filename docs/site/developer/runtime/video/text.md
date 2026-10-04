# Text layer: GameText

**What you will learn:** how `GameText` keeps the text map and the glyph pixels, what each text hook reads from the game, and why the layer is supported only when every glyph in use is complete.

The files are `runtime/game_text.hpp` and `runtime/game_text.cpp`.

## What the class holds

The game draws text with 8x8 glyphs. A map of 64x64 cells (512x512 pixels) names the glyph of each cell. The glyph pixels can change at run time, because the game fades text by changing glyph 0x90.

| Member | Type | Meaning |
| --- | --- | --- |
| `cells_` | `std::array<Cell, 4096>` | The map. `Cell` has `tile` (glyph number), `palette` (0 to 63), `flip_x`, `flip_y`. |
| `glyphs_` | `std::array<uint8_t, 256 * 64>` | The pen (0 to 15) of each pixel of each glyph |
| `glyph_rows_` | `std::array<uint8_t, 256>` | For each glyph, one bit per pixel row. A set bit means the game uploaded that row. A glyph is **complete** when the byte is 0xff. |
| `references_` | `std::array<uint16_t, 256>` | The number of map cells that use each glyph |
| `map_valid_` | `bool` | True when the map is known |
| `unsupported_pc_` | `uint32_t` | The first PC that made the layer unsupported (for reports) |

`reset()` clears everything, sets `references_[0]` to 4096 (all cells use glyph 0) and makes the map invalid. `clear()` (the hook at 0x59bc) sets all cells to `{tile 0x90, palette 1}`, sets `references_[0x90]` to 4096 and makes the map valid.

## Why `supported()` checks the glyphs

```cpp
bool GameText::supported() const {
    if (!map_valid_) return false;
    for (unsigned tile = 0; tile < 256; ++tile)
        if (references_[tile] && glyph_rows_[tile] != 0xff) return false;
    return true;
}
```

The layer is supported when the map is known **and** every glyph that a cell uses has all eight pixel rows from a modeled upload. `references_` is a counter updated by `put`. A glyph that no cell uses does not matter, even if its pixels are unknown. This lets the layer stay supported at the start of the game, before the game uploads the glyphs it does not use.

## Hooks

Stack offsets are from `A7` at routine entry. `sp` below means `A7`.

### Map writers

| PC | Reads | Action |
| --- | --- | --- |
| 0x56e6 | Source `sp+4`; destination `sp+8`; attribute word `sp+12`. The source has `u16 rows`, `u16 columns`, then one `u16` glyph for each cell. | Writes `(attribute << 8) \| glyph` to each cell, row by row. The destination moves 128 bytes (one map row) for each row. |
| 0x5726 | Same arguments. The source holds one **byte** for each glyph. | Like 0x56e6, but the column order is reversed (`column = width - 1 - x`) and the attribute is XORed with 0x40. The row and column counts use the signed-word test (`tested_word_count`). |
| 0x5768 | Source (header only), destination | Erases the described rectangle: glyph 0 and attribute 0. |
| 0x57a2 | String pointer `sp+4`; destination `sp+8`; palette `sp+12` | Writes one cell for each byte until the NUL byte. The attribute byte is `palette * 2`. |
| 0x57cc | Value `sp+4` (long); digit count `sp+8`; destination `sp+10`; palette `sp+14` | Hexadecimal number. Writes right to left. The attribute is `(palette * 2) & 0x3e`. A digit below 10 is glyph `0x30 + digit`, else `0x37 + digit`. |
| 0x5856 | Same | Decimal number. Right to left. The attribute is `(palette * 2) & 0x7e`. A count of 0 means 65536 digits. |
| 0x581c | Same | Bit display. Left to right. For bit `n - 1` of the value: glyph `0x48` (`H`) if set, else `0x4c` (`L`). |
| 0x59bc | None | `clear()` (see above). |
| 0x59ee | None | Fill 30 rows by 40 columns with the value 0x0290. |
| 0x5be0, 0x5c08 | None | Write 29 rows of the first column (value 0x0201) or the second column (value 0x0401). |
| 0x9b544 | None | Write the value 0x0260 to 42 columns in rows 0 to 5 and rows 26 to 28. |

### Glyph writers

| PC | Reads | Action |
| --- | --- | --- |
| 0x5b80, 0x5bac | `A0` source, `D0.W + 1` rows of 32 bits | Upload glyph rows from glyph 0 at 0x61e000. Swap the two 16-bit halves of each long before nibble decoding. Then `solid_glyph(0x90, 0)`. |
| 0x5bce | Same | Same upload, but from glyph 0xa0 (address 0x61f400). It does not clear glyph 0x90. |
| 0x8de56 | None | `solid_glyph(0x90, 0)`: blank glyph 0x90 |
| 0x8e9c6, 0xa1170 | None | `solid_glyph(0x90, 15)`: fill glyph 0x90 |
| 0x9b530 | None | `solid_glyph(0x60, 15)` |
| 0x8e0a6, 0x8e0dc | `A1` points to two (offset, mask) pairs; `A0` is the glyph base | For each pair `glyph_mask(A0 + offset, mask, set)`. 0x8e0a6 sets bits (`pen \|= bits ^ 15`). 0x8e0dc clears bits (`pen &= bits`). Each mask covers four pixels. |

The functions work on the semantic glyph, not on character RAM bytes. `glyph_row` splits a 32-bit value into eight 4-bit pens (lowest nibble is pixel 0) and marks the pixel row. `solid_glyph` fills 64 pixels and marks all 8 rows.

After the `switch`, `observe` checks `memory.supported`. If a hook read an address outside ROM and work RAM, it sets `map_valid_` to false and stores the PC.

## Write guard

`observe_write(pc, address)` watches 0x61c000 to 0x61ffff. If `pc` is not one of the 40 store PCs that the hooks model (see [Producer hooks](/developer/runtime/video/producers)), then:

- for an address below 0x61e000 (the map), it sets `map_valid_` to false;
- for a glyph address, it clears `glyph_rows_` for that glyph. The glyph is then incomplete.

In both cases it stores the PC if none is stored yet. A later known upload can make the glyph complete again. Only a `clear()` makes an invalid map valid again.

## Pixel sampling

`pixel(x, y, flipped)`:

1. `x &= 511` and `y &= 511`. If flipped, `x = 511 - x` and `y = 511 - y`.
2. The cell is `cells_[(y / 8) * 64 + x / 8]`.
3. The pixel position inside the glyph is `(x & 7)` and `(y & 7)`, each XORed with 7 for a flip.
4. The result is `palette = cell.palette * 16 + pen` and `flags = pen ? 0x10 : 0`.

The compositor calls it with `row.text_x` and `row.text_y`, which `GameLines::prepare` computes from the pivot scroll words.

## Invariants

- Keep `references_` correct. `put` decrements the old glyph count and increments the new count. A mistake here changes when the layer is supported.
- A new glyph writer must mark the pixel rows it fills. Without this the glyph stays incomplete.
- The layer has no bitmap mode. A frame with `bitmap` set in any row falls back (see [GameVideo](/developer/runtime/video/game-hle)).

## Unit check

No unit check exists for text in `runtime/check.cpp`. The proof is the text-layer comparison of the gameplay regression with layer mask 256, which compares complete 512x512 indexed textures. See [Parity evidence and limits](/developer/runtime/video/parity).

Sources: [game_text.hpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_text.hpp) and [game_text.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_text.cpp).
