---
title: FDP playfields (PF1-PF4 tilemaps)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, tilemap, line-ram, memory-map, emulator-bug, undocumented]
sources: [raw/docs/taito-f3/graphics-structs.txt, raw/docs/taito-f3/graphics.txt, raw/docs/taito-f3/scroll-regs.txt, raw/docs/taito-f3/website/fdp/tilemap.html, raw/docs/taito-f3/website/fdp/lineram.html, raw/docs/taito-f3/website/fdp-memory.html, raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/terms.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: [0x00610000, 0x00660000, 0x00660008, 0x0066001E]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/graphics-structs.txt, note: "tile word layouts and address decoding"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/tilemap.html, note: "scroll/zoom maths and register field tables (draft page)"}
  - {kind: doc, ref: raw/docs/taito-f3/line-ram.txt, note: "line-RAM fields that feed the playfields"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "get_tile_info, get_pf_scroll, read_line_ram, x_index/y_index"}
contradictions:
  - "Tile flip bits: graphics-structs.txt gives $80000000 = horizontal flip, $40000000 = vertical; fdp-memory.html lists orientation 01 = vertical, 10 = horizontal; MAME feeds bits 15:14 of the first word straight to TILE_FLIPYX (bit 14 = X, bit 15 = Y), i.e. swapped relative to the notes."
  - "Alternate tile data in extend mode: notes' extend-mode address allows tilemap numbers 4-5 (alternate maps); MAME honours the line-RAM alternate-tilemap bit only when extend mode is off."
  - "Y scroll sync: tilemap.html and line-ram.txt describe a per-scanline sync bit (6000 bit 11) that reloads the Y accumulators; MAME does not read it and reloads the accumulators once per frame (before scanline 0)."
  - "Vertical zoom of PF1/PF3: notes call the swapped registers a hardware 'BUG'; MAME implements the swap (FIX_Y = {0,3,2,1}). No disagreement on the effect, listed because it is a trap for any renderer."
  - "tilemap.html: Y scroll register address labelled $660004+ (word index) while scroll-regs.txt writes $660008 (byte address); same location."
supersedes: []
---

# FDP playfields

Four tilemap layers (PF1-PF4, "tilemap 0-3" in 12Me21's numbering), part of the [[hardware/fdp]].
Each is a 32x32 grid of 16x16 px tiles, or 64x32 when *extend mode* is enabled (register 0x66001E bit
7). Each tile is one 32-bit value holding a ROM texture id, palette, flips, bit depth and a blend
select. ^[raw/docs/taito-f3/website/fdp/tilemap.html] ^[raw/docs/taito-f3/scroll-regs.txt]
MAME describes the same thing as 512x512 or 1024x512 pixel layers of 4/5/6 bpp tiles, two of which
have line-by-line access to alternate tilemaps. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Scaling: vertical 50.2% to infinity, horizontal 100% to 25600% (see below).
^[raw/docs/taito-f3/website/fdp/tilemap.html]
Related: [[hardware/line-ram]], [[hardware/priority-and-blend]], [[hardware/clip-and-mosaic]],
[[hardware/pivot-layer]].

## Tile RAM layout

Bus base 0x00610000 (graphics RAM `[...01 ...]`). The tilemap number `n` is 3 bits. Maps 0-3 are the
four playfields; maps 4 and 5 are the "alternate" data for PF3 and PF4 (tilemap 2 and 3), "add 2 for
alt tilemap". ^[raw/docs/taito-f3/graphics-structs.txt]

| Mode | Address pattern (offset from 0x600000) | x | y | n | Total |
|---|---|---|---|---|---|
| normal (32x32) | `[...01 0nnn yyyy yxxx xx--]` | bits 6:2 (0-31) | bits 11:7 | bits 14:12 | 0x1000 per map |
| extend (64x32) | `[...01 nnny yyyy xxxx xx--]` | bits 7:2 (0-63) | bits 12:8 | bits 15:13 | 0x2000 per map |

(The mask list in `graphics-structs.txt` gives `$000700` for the normal-mode `n`; that overlaps `y`
and probably is a typo for `$7000`, which is what the bit pattern above says.)
^[raw/docs/taito-f3/graphics-structs.txt]

^[raw/docs/taito-f3/graphics-structs.txt] `fdp-memory.html` agrees: normal mode 0x610000 + n*0x1000,
extend mode 0x610000 + n*0x2000 for the four large maps, and the alternate maps follow at
0x618000-0x61BFFF (that page labels them inconsistently, e.g. "tilemap 3 alternate", "tilemap 4 alternate" and "large
tilemap 2 alternate" twice, because of 0/1-based numbering).
^[raw/docs/taito-f3/website/fdp-memory.html]
MAME's RAM: normal mode eight 0x1000-byte maps (`m_pf_data[i] = pf_ram + 0x1000*i/2`), extend mode
four 0x2000-byte maps; there is no alternate-map storage in extend mode. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Tile index within a map is row-major (`TILEMAP_SCAN_ROWS`). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Tile data (two 16-bit words, big endian)

First word at the lower address, second word at +2. 32-bit view `{xytt ppvc cccc cccc}{tttt tttt tttt tttt}`:
^[raw/docs/taito-f3/graphics-structs.txt]

| 32-bit mask | Field | Meaning |
|---|---|---|
| 0x0000FFFF | t | texture (tile) index low 16 bits (word 1) |
| 0x30000000 | t high | upper 2 bits of the tile index ("upper bits unemulated" in the notes) |
| 0x01FF0000 | c | palette row, 9 bits (0-511) |
| 0x02000000 | v | per-tile blend (alpha) select |
| 0x0C000000 | p | texture planes / bits per pixel: 00 = 4bpp, 01 = 5bpp, 10 = "?", 11 = 6bpp |
| 0x40000000 | y | flip (notes: vertical) |
| 0x80000000 | x | flip (notes: horizontal) |

^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/website/fdp/tilemap.html]
MAME: `[yx?? ddac cccc cccc]`: palette = bits 8:0, blend select = bit 9, extra planes = bits 11:10,
flip = bits 15:14 (see contradictions), tile number = word 1 only; bits 13:12 are "upper bits of tile
number?". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Texture id = word1 | tex2 << 16 per `tilemap.html` ("Texture Id = texture | tex2"); tile texture
reads use 18 bits of the graphics bus address, 32 bus addresses of 48 bits per tile.
^[raw/docs/taito-f3/website/fdp/tilemap.html] ^[raw/docs/taito-f3/sprite-ram.txt]
The final colour index is `texture colour | palette << 4`; palette bits overlap the 5/6bpp high plane
bits and are combined with OR (MAME masks the planes with `(extra & ~palette) << 4 | 0x0f` to
imitate this). ^[raw/docs/taito-f3/terms.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Blend select is stored per tile here, per line for every other layer. ^[raw/docs/taito-f3/website/fdp/tilemap.html]

## Global scroll registers

Write-only, at 0x660000 (see [[hardware/fdp]] for the register block). ^[raw/docs/taito-f3/scroll-regs.txt]

| Register | Address | Format |
|---|---|---|
| PF n horizontal scroll (n = 0..3) | 0x00660000 + 2n | `[iiii iiii iiff ffff]`; integer part 10 bits (0-1024), fraction in 1/64; "f - negative fractional part"; scroll (texture pixels) = ipart - fpart/64 |
| PF n vertical scroll | 0x00660008 + 2n | tilemap.html: integer 9 bits (0-511), fraction 7 bits in 1/128; scroll = ipart + fpart/128; sign of the fraction not known ("is this negative like x scroll?") |

^[raw/docs/taito-f3/scroll-regs.txt] ^[raw/docs/taito-f3/website/fdp/tilemap.html]
MAME stores X as "fixed10.6 with fractional bits inverted" and Y as 9.7, converting both to 8-bit
fixed point; it adds a per-playfield X offset of `(40 - 4*n) << 6`, a Y offset of one line (`1<<7`),
and the horizontal start of the visible area (H_START = 46). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
MAME's code comment asks "why don't we need to do the 24 adjustment for pf 1 and 2 ?", so the constants are
empirical. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Per-scanline controls (line RAM)

All are line-RAM fields; offsets and latching in [[hardware/line-ram]]. Names follow the notes.
^[raw/docs/taito-f3/website/fdp/tilemap.html]

| Field | Line register | Bits | Applies to | Format |
|---|---|---|---|---|
| X scroll adjust ("rowscroll") | 6.n `A000/A200/A400/A600` | 15:0 | PF1-4 | same format as the X global scroll register |
| Y scroll adjust ("colscroll") | 0.2 `4400`, 0.3 `4600` | 8:0 | PF3 and PF4 only (tilemaps 2, 3) | integer texture pixels, 0-511 |
| alternate tile data | 0.2/0.3 | 9 | PF3, PF4 only | 0 = normal map, 1 = alternate map (maps 4/5) |
| X zoom | 4.n `8000-8600` | 15:8 | PF n | texture px per screen px = 1 - xzoom/256 |
| Y zoom | 4.n | 7:0 | PF n (PF1 and PF3 swapped) | texture px per screen px = yzoom/128 |
| palette adjust ("palette add") | 5.n `9000-9600` | 5:0 | PF n | added to palette id of all tiles (only 6 bits) |
| Y scroll sync | 2.0 `6000` | 11 | all playfields | 1 = reload Y accumulators from the Y global scroll registers |
| mosaic enable | 2.2 `6400` | 3:0 | PF n | see [[hardware/clip-and-mosaic]] |
| priority/clip/blend/enable | 7.n `B000-B600` | 15:0 | PF n | see [[hardware/priority-and-blend]] |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html] ^[raw/docs/taito-f3/website/fdp/tilemap.html]
Section 0 lacks its first two subsections (`4000`, `4200`), so Y scroll adjust and alternate tilemap
exist only for tilemaps 2 and 3. ^[raw/docs/taito-f3/website/fdp/lineram.html]

Example scale factors (screen px per texture px): ^[raw/docs/taito-f3/website/fdp/tilemap.html]

| Raw | X zoom | Y zoom |
|---|---|---|
| `$00` | 1.0x | infinity |
| `$40` | 1.333x | 2.0x |
| `$80` | 2.0x | 1.0x |
| `$C0` | 4.0x | 0.667x |
| `$FF` | 256.0x | 0.502x |

The notes' zoom table is in screen pixels per texture pixel; `lineram.html` gives the same
quantities as "factor = 1 / (1 - zoom/256)" for X and "1 / (zoom/128)" for Y.
^[raw/docs/taito-f3/website/fdp/tilemap.html] ^[raw/docs/taito-f3/website/fdp/lineram.html]

### PF1/PF3 vertical zoom swap

The Y zoom of playfields 1 and 3 is stored in each other's line-register word: register `8200` holds
PF1's X zoom and **PF3**'s Y zoom, `8600` holds PF3's X zoom and **PF1**'s Y zoom. Registers `8000`
and `8400` are unswapped. The notes mark this "BUG". ^[raw/docs/taito-f3/line-ram.txt]
^[raw/docs/taito-f3/website/fdp/lineram.html] MAME also notes "playfield 2 & 4 registers seem to be
interleaved" using different numbering (its playfield 2 = tilemap 1). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
See [[quirks/tilemap-y-zoom-pf1-pf3-swapped]].

## Scroll and zoom maths

X (per scanline `y`, screen pixel `x`, tilemap number `tnum`), texture-pixel source X:
`Xglobal + Xadjust[y] + 18 + (x + 22 - 4*tnum) * Xzoom[y]`. ^[raw/docs/taito-f3/website/fdp/tilemap.html]
(The constants 18, 22 and 4*tnum are the notes' empirical fit; the x origin of the screen is
undetermined, "TODO: what is the origin of the screen?".) ^[raw/docs/taito-f3/terms.txt]

Y has a per-tilemap *Y scroll accumulator* in texture pixels (1/128 precision). At the start of each
scanline, if Y scroll sync is set (usually on scanline 0) the accumulator is loaded from the Y global
scroll register; otherwise it is incremented by the Y zoom register value; then the scanline is
rendered with source row = accumulator + Y scroll adjust. The accumulator is not reset at frame
start and carries over until sync is set. Hence
`src_y = Yglobal + Yadjust[y] + sum over j in [sync, y) of Yzoom[j]` (texture pixels). ^[raw/docs/taito-f3/website/fdp/tilemap.html]
Games must therefore set the sync bit on the first scanline or the playfield Y position is unstable.
^[raw/docs/taito-f3/website/fdp/tilemap.html]

MAME's model: X index = `((reg_fx_x + (x - H_START) * x_scale) >> 8 + H_START) & width_mask` with
`reg_fx_x = reg_sx + rowscroll + 10 * (x_scale - 256)`; Y index = `((reg_fx_y >> 8) + colscroll) & 0x1ff`,
where `reg_fx_y` starts at the Y scroll and gains `y_scale` after every scanline except line 0; the
rowscroll word is treated as 10.6 fixed, `(i<<8) - (f)`, "fractional part is negative (allegedly)".
^[raw/emu-source/mame-0.289/taito_f3_v.cpp] When flipscreen is set MAME negates Y and adds extra offsets
to X (`320<<6` and `(512+192)<<6`) and reads line RAM in reverse. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Notes for a runtime

- Tile data is just RAM; the renderer needs `extend` (bit 7 of 0x66001E) to pick the address decode
  above. MAME hard-codes extend per game instead of reading the register, so a per-game table there
  is a hint, not a hardware rule. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Hypothesis: because setting the lock bit (bit 0) clears extend mode and ignores later writes, a
  game that wants extend mode must set bit 7 before locking. No game behaviour is documented.
  ^[raw/docs/taito-f3/scroll-regs.txt]
