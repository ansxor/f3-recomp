---
title: FDP pivot layer (text / pixel layer)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, text, tilemap, line-ram, memory-map, undocumented]
sources: [raw/docs/taito-f3/website/fdp/pivot.html, raw/docs/taito-f3/website/fdp/home.html, raw/docs/taito-f3/website/line-ram.html, raw/docs/taito-f3/graphics.txt, raw/docs/taito-f3/graphics-structs.txt, raw/docs/taito-f3/terms.txt, raw/docs/taito-f3/scroll-regs.txt, raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/website/fdp-memory.html, raw/docs/taito-f3/website/fdp/sprite.html, raw/docs/taito-f3/website/new-test.html, raw/emu-source/mame-0.289/taito_f3_v.cpp, raw/emu-source/mame-0.289/taito_f3.cpp]
games: []
addresses: [0x0061C000, 0x0061E000, 0x00620000, 0x00630000, 0x00660018, 0x0066001A]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/pivot.html, note: "text vs pixel mode, addresses, line-register fields"}
  - {kind: doc, ref: raw/docs/taito-f3/graphics-structs.txt, note: "text tile / glyph row / pixel glyph row address and data layouts"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "get_tile_info_text/_pixel, pivot scroll, use_pix(); gfx_layout in taito_f3.cpp"}
contradictions:
  - "Mode/bank bits in line register 6000: notes say bit 13 = pixel mode and bit 15 = pixel data bank; MAME's use_pix() is true when either bit 13 or bit 15 of the word is set (pivot_control & 0xa0)."
  - "Mosaic: line-ram.txt's header says 'bit 9: NOT pivot mosaic' while its 6400 table describes bit 9 as the highcolor ('mystery') layer's mosaic enable; line-ram.html and MAME make bit 9 the pivot mosaic enable; graphics.txt says mosaic is not supported by the pivot layer. See [[hardware/line-ram]]."
  - "Pixel order inside a glyph row: graphics-structs.txt writes a text glyph row as 8 nibbles `aaaa bbbb ... hhhh` in order; MAME's charlayout/pivotlayout scramble the nibbles (bit offsets 20,16,28,24,4,0,12,8). The notes say the pixel-layer pixels are 'in a weird order'; they do not state the order."
supersedes: []
---

# FDP pivot layer

One of the eight layers of the [[hardware/fdp]]. A 64x64 grid of 8x8 px tiles (512x512 px) whose
textures live in **RAM** instead of the cartridge ROM. The name's origin is unknown ("we're not sure
what the name means"). It is "per-tile palette and flipping, global scroll, per-scanline 'pivot
port' (unknown feature)". ^[raw/docs/taito-f3/website/fdp/home.html] ^[raw/docs/taito-f3/website/fdp/pivot.html]
Compositing parameters (priority, clip, blend) are in [[hardware/line-ram]] and
[[hardware/priority-and-blend]]; the layer is also called the "VRAM layer" in MAME. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Two modes (selected per scanline)

| | text mode | pixel mode |
|---|---|---|
| texture lookup | tile's 8-bit texture id picks one of 256 glyphs in the font RAM | each cell has its own texture whose address depends only on the cell position |
| picture | 64x64 tiles | effectively a 512x256 4bpp bitmap |
| attributes | palette and flips from text RAM | same text RAM entries |

^[raw/docs/taito-f3/graphics.txt] ^[raw/docs/taito-f3/website/fdp/pivot.html]
In pixel mode the bottom half (rows 32-63) uses the same textures as the top half but its own
attributes, because attributes always come from the 64x64 text RAM (a 512x512 region). So if the
pixel layer is scrolled vertically past 256 lines, the layer wraps to a second copy drawn with the
other half's palette and flip data. ^[raw/docs/taito-f3/graphics.txt]
^[raw/docs/taito-f3/terms.txt] MAME reproduces this with a hack: the pixel layer picks text-RAM rows
0-31 or 32-63 depending on the pivot Y scroll and flipscreen at tile-dirty time (and "we SHOULD dirty
parts of the pixel layer if scroll or flipscreen changes... but we don't"). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## RAM layout

| Bus range | Contents | Source |
|---|---|---|
| 0x0061C000-0x0061DFFF | text RAM: 64x64 words (the "tile" attribute entries) | ^[raw/docs/taito-f3/website/fdp-memory.html] |
| 0x0061E000-0x0061FFFF | character font: 256 glyphs x 8 rows x 1 long (4bpp 8x8) | ^[raw/docs/taito-f3/website/fdp-memory.html] |
| 0x00630000-0x0063FFFF | pixel-mode texture data, bank 1 (64x32 cells x 8 rows of longs) | ^[raw/docs/taito-f3/website/fdp/pivot.html] |
| 0x00620000-0x0062FFFF | pixel-mode texture data, bank 0 — "collides with lineram" | ^[raw/docs/taito-f3/website/fdp/pivot.html] ^[raw/docs/taito-f3/line-ram.txt] |

MAME maps text RAM 0x61C000-0x61DFFF, char RAM 0x61E000-0x61FFFF (256 glyphs) and pivot RAM
0x630000-0x63FFFF (2048 glyphs = 64x32 cells), and has no model of the 0x620000 bank.
^[raw/emu-source/mame-0.289/taito_f3.cpp] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

### Text tile (text RAM entry)

Address `[...01 110y yyyy yxxx xxx-]` (x = bits 6:1, 0-63; y = bits 12:7, 0-63), so word index =
`y << 6 | x` from 0x61C000. ^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/website/fdp/pivot.html]

| Bits | Field | Meaning |
|---|---|---|
| 15 | y | vertical flip |
| 14:9 | c | palette row (0-63) |
| 8 | x | horizontal flip |
| 7:0 | t | texture (glyph) id, 0-255; used only in text mode |

^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/website/new-test.html] MAME decodes
`[yccc cccx tttt tttt]` identically. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
`new-test.html` is a scratch page from which this bit-picture notation was tuned; it contains the
same layout. ^[raw/docs/taito-f3/website/new-test.html]

### Text glyph row

Address `@[...01 111t tttt tttr rr--]`: glyph id = bits 12:5, row = bits 4:2; one 32-bit long per row;
4bpp, 8x8; 32 bytes per glyph. ^[raw/docs/taito-f3/graphics-structs.txt]

### Pixel-mode glyph row

Address `[...1b xxxx xxyy yyyr rr--]`: `b` = bit 16 bank, x = bits 15:10 (0-63), y = bits 9:5 (0-31; "layer
y index / 8"), r = row bits 4:2; one long per row, 32 bytes per cell, 64 KiB per bank. The cell
order is column-major: a cell's index is `x*32 + y`, matching
MAME's `TILEMAP_SCAN_COLS` 64x32 pixel layer. ^[raw/docs/taito-f3/graphics-structs.txt]
^[raw/docs/taito-f3/website/fdp/pivot.html] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Each long holds eight 4-bit pixels. Per MAME's `charlayout`/`pivotlayout` the pixel bit offsets in
the 32-bit row are `{20, 16, 28, 24, 4, 0, 12, 8}` (counted from the most significant bit of the
big-endian long, rows `STEP8(0, 4*8)`). ^[raw/emu-source/mame-0.289/taito_f3.cpp] Hypothesis (my
derivation from that table, not stated by either source): pixel 0..7 occupy long bit ranges
11:8, 15:12, 3:0, 7:4, 27:24, 31:28, 19:16, 23:20 respectively. The 12Me21 notes just say the pixels
are "in a weird order" and the font page gives no order. ^[raw/docs/taito-f3/website/fdp/pivot.html] ^[raw/docs/taito-f3/graphics.txt]
Because they are RAM, the glyph and pixel data are rewritten by the CPU; MAME marks the decoded gfx
dirty on every write. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Scroll registers

Write-only, 10 bits significant: pivot X at 0x00660018, pivot Y at 0x0066001A, format
`[.... ..ss ssss ssss]`. ^[raw/docs/taito-f3/scroll-regs.txt] MAME: X index = `(x + reg_sx) & 0x1ff`;
Y index = `(reg_sy + y) & 0xff` in pixel mode (256 lines high) or `& 0x1ff` in text mode, with
`reg_sx = -control_1[4] - 5` and `reg_sy = -control_1[5]` (flipscreen: `control_1[4] - 12`,
`control_1[5]`). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] See [[hardware/fdp]] for the register block.

## Line-RAM fields

| Register | Bits | Field |
|---|---|---|
| `6000` (2.0) | 15 | pixel-mode data bank: 0 = 0x620000 (collides with line RAM), 1 = 0x630000 |
| `6000` | 13 | tile mode: 0 = text, 1 = pixel |
| `6000` | 9 | pivot alpha (blend value) select |
| `6400` (2.2) | 9 | pivot mosaic enable per line-ram.html/MAME (see contradictions) |
| `7200` (3.1) | 15 | blend mode: 0 = both half-pixels (A+B), 1 = A ("left"); different interpretation than other layers |
| `7200` | 13:4 | clip settings: enable (13), invert (12), 4 plane enables (11:8), 4 plane modes (7:4) |
| `7200` | 3:0 | priority |
| `7000` (3.0) | 15:0 | MAME: "pivot enable" (unemulated); website/line-ram.html labels it "pivot/vram layer enable??"; website/fdp/lineram.html uses the same word for the highcolor layer's priority/clip/blend |
| pivot port latch 0x621000 + 2*scanline | 7:4 | 4 latch bits for subsections of the "pivot port" (unknown which) |

^[raw/docs/taito-f3/website/fdp/pivot.html] ^[raw/docs/taito-f3/line-ram.txt]
^[raw/docs/taito-f3/website/line-ram.html] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Details of every field are on [[hardware/line-ram]] (that page also treats the 0x6000 bit assignments
inconsistently between the sources). MAME's `7200` handling ("pivot layer mix info word") is the same
`BAEI cccc iiii pppp` word as the playfield mix words. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

The "pivot port" is a separate region 0x621000-0x62FFFF of line RAM with an unknown purpose: the
notes list a per-scanline latch word at 0x621000 (`[.... .... llll ....]`, which subsection each
latch controls is unknown "because there's more than 4 subsections") and subsections at
`[*010 0001 uuul llll lll-]`, `u` in 1-7, data "unknown". MAME: "'Pivot port' (0x1000-2fff) has only
one known used address. 0x1000: unknown control word? (usually 0x00f0; gseeker, spcinvdj, twinqix,
puchicar set 0x0000)". ^[raw/docs/taito-f3/line-ram.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
(MAME's 0x1000-0x2FFF are offsets within line RAM, i.e. 0x621000-0x622FFF.) Per-line, the notes'
latch word equals MAME's usual `0x00f0`, which suggests (hypothesis) that the four set bits are the
latches that games leave on. ^[raw/docs/taito-f3/line-ram.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Sprite-command interaction

Sprite settings bit 12 (`j`): "if this is 1, the lowest bit of the texture id of all pivot layer
tiles is treated as inverted (e.g. 44->45, 45->44)", and the video timing changes slightly; it must
be set near the start of the frame (sprite index <= 16). See [[hardware/sprites]].
^[raw/docs/taito-f3/website/fdp/sprite.html]

## Unknown / not covered

- Bit order of the pixel layer's 4bpp data is not documented by the notes (they only say "in a weird order"). ^[raw/docs/taito-f3/website/fdp/pivot.html]
- Pixel-mode data bank 0 lives at 0x620000 and therefore overlaps the line-RAM latch table and
  sections; the notes only say it "collides" and give no usage examples.
  ^[raw/docs/taito-f3/website/fdp/pivot.html]
- The notes write "pixel glyph row" bit masks `$00FFE0` for both `x` and `y` in `graphics-structs.txt`
  (a transcription slip; the html page's separate x:6, y:5 fields are used above).
  ^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/website/fdp/pivot.html]
