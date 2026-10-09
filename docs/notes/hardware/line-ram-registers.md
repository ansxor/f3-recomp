---
title: FDP line registers (bit fields of the 30 per-scanline registers)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, line-ram, blend, clip, tilemap, sprite, palette, emulator-bug, undocumented]
sources: [raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/website/fdp/lineram.html, raw/docs/taito-f3/website/line-ram.html, raw/docs/taito-f3/website/fdp/layer.html, raw/docs/taito-f3/website/fdp/sprite.html, raw/docs/taito-f3/website/fdp/tilemap.html, raw/docs/taito-f3/website/fdp/pivot.html, raw/docs/taito-f3/website/fdp/highcolor.html, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/line-ram.txt, note: "raw per-register bit descriptions from hardware tests"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/lineram.html, note: "website table of the same registers"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "header comment and read_line_ram()"}
contradictions:
  - "6400 bit 9: line-ram.txt table = 'mystery layer' mosaic enable (header line: 'NOT pivot mosaic'); fdp/lineram.html and highcolor.html = highcolor layer mosaic enable; website/line-ram.html and MAME = pivot mosaic enable."
  - "6200 nibble roles: line-ram.txt `[LLLL llll RRRR rrrr]` and lineram.html `[B1 B0 A1 A0]` give the same layout (bits 3:0 = right/A, select 0; 7:4 = right/A, select 1; 11:8 = left/B, select 0; 15:12 = left/B, select 1) but this makes A = right, while the blend-mode fields elsewhere in the same files use 1 = left = A. MAME labels bits 3:0 'a' = DEST contribution A."
  - "6000 low byte blend-mode meaning: line-ram.txt 0=neither,1=left,2=right,3=both; lineram.html 0 none,1 A,2 B,3 both. These match if A = left. MAME sprite layer_enable: blend mode 0 means disabled."
  - "7000: line-ram.html and MAME ('pivot enable', unemulated) treat it as a pivot/vram enable word; fdp/lineram.html and highcolor.html give it highcolor-layer priority/clip/blend."
  - "Y scroll sync bit (6000 bit 11): documented by tilemap.html/line-ram.txt but not read by MAME."
  - "Sprite mix word 7400: 12Me21 shows `AAAA ??EI cccc iiii` (bits 11:10 unknown); MAME logs bits 11:10 as unknown and notes 0x0800 is set by many games (bit 11)."
supersedes: []
---

# FDP line registers

Bit fields of each line register. Structure, addressing and latching are on [[hardware/line-ram]];
the chip is the [[hardware/fdp]]. Bit numbers are from the MSB-first patterns in the notes (counted
by the wiki); values in `$`/`0x` are hex. Every row is cited by the section's source line below it.

## 4400 / 4600 (0.2, 0.3): PF3/PF4 column scroll, alt tilemap, clip high bits

Pattern `[RLrl ??Ts ssss ssss]`; 4600 is identical for PF4 and clip planes 2 and 3.
^[raw/docs/taito-f3/line-ram.txt]

| Bits | Field | Meaning |
|---|---|---|
| 15 | R | clip plane 1 right, high bit |
| 14 | L | clip plane 1 left, high bit |
| 13 | r | clip plane 0 right, high bit |
| 12 | l | clip plane 0 left, high bit |
| 9 | T | 1 = use alternate tilemap (PF3: tilemap 4, PF4: tilemap 5) |
| 8:0 | s | local Y scroll ("colscroll") in integer texture pixels |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
MAME: colscroll = `word & 0x1ff`, alt tilemap = `word & 0x200` and only when extend mode is off;
clip plane upper bits taken from bits 12-15. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Consumers: [[hardware/tilemaps]], [[hardware/clip-and-mosaic]].

## 5000-5600 (1.0-1.3): clip plane low bits

`[rrrr rrrr llll llll]`: bits 15:8 right edge low 8 bits, bits 7:0 left edge low 8 bits of clip planes
0, 1, 2, 3 for 5000, 5200, 5400, 5600; units are screen pixels; the 9th bit is in 4400/4600.
^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
MAME comment: "renderer needs to adjust clip by -48" (and it uses `l-1` / `r-2`).
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## 6000 (2.0): sprite blend modes, alpha selects, pivot mode, Y sync

Pattern `[B?p? oNAm ddcc bbaa]` (line-ram.txt); website/fdp/lineram.html writes `B,?,P,?,y,b,p,h,s3..s0`.
^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]

| Bits | Field | Meaning |
|---|---|---|
| 15 | B | pivot pixel-mode data location: 0 = 0x620000 (collides with line RAM), 1 = 0x630000 |
| 14 | ? | unknown |
| 13 | p/P | pivot mode: 1 = pixel, 0 = text |
| 12 | ? | unknown |
| 11 | o/y | "playfields / mystery layer reset Y source index" = tilemap Y scroll sync (see [[hardware/tilemaps]]); MAME comment: "0x0800 seems to set the vram layer to be opaque [unemulated]" |
| 10 | N/b | background colour blend (alpha) value select |
| 9 | A/p | pivot alpha select |
| 8 | m/h | highcolor ("mystery") layer alpha select |
| 7:6 | d | sprite group 3 blend mode |
| 5:4 | c | sprite group 2 blend mode |
| 3:2 | b | sprite group 1 blend mode |
| 1:0 | a | sprite group 0 blend mode |

Blend mode of a sprite group: 0 = neither, 1 = left (A), 2 = right (B), 3 = both.
^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/sprite.html]
MAME treats the upper byte as `pivot_control` (`B?p? o?A?`, "p = enable pixel layer" = bit 13, "garbage
pixels for this line" = 0x2000 unemulated) and the low byte as `DdCc BbAa` blend modes for
sprite groups with palette top bits 0xC0/0x80/0x40/0x00. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Detail on blend modes: [[hardware/priority-and-blend]].

## 6200 (2.1): the four blend values

`[LLLL llll RRRR rrrr]` in line-ram.txt; `[B1 B0 A1 A0]` nibbles in lineram.html. Each nibble is
an alpha value: `alpha = clamp((15 - value)/8, 0, 1)`; `$0-$7` = 100%, `$B` = 50%, `$F` = 0%.
^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]

| Bits | line-ram.txt | lineram.html |
|---|---|---|
| 3:0 | r: right pixel, blend select 0 | A0: half-pixel A, select 0 |
| 7:4 | R: right pixel, blend select 1 | A1: half-pixel A, select 1 |
| 11:8 | l: left pixel, blend select 0 | B0: half-pixel B, select 0 |
| 15:12 | L: left pixel, blend select 1 | B1: half-pixel B, select 1 |

(Reading the two files together, "A" = "right" here, whereas the blend-mode fields elsewhere in the
same files use 1 = left = A; the naming is therefore unresolved, only the nibble positions are
consistent.) ^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
MAME: nibble `idx` -> `min(8, 15 - alpha)`, i.e. `0x7` and below = 8/8.
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## 6400 (2.2): mosaic, shadow, FDA mode

| Bits | line-ram.txt (`ffB? Sgps mmmm 3210`) | lineram.html | MAME (`?wBu` + `??ps mmmm 4321`) |
|---|---|---|---|
| 15:14 | f: FDA mode (0 = 15-bit colour, 1 = normal, 2 = double resolution/no blending, 3 = invalid?, repeats first pixel) | fda: same | bit 15 ?; bit 14 w = 12-bit RGB palette [unemulated] |
| 13 | B: horizontal blur, 0 = enable, 1 = disable | blur:2 at 13:12; 0,1 blurred, 2,3 normal | B: 0 = enable forward blur |
| 12 | ? | (part of blur) | u: "normally 1" |
| 11 | S: shadow mode on | sh:2 at 11:10: 0,1 off; 2 = shadow 2; 3 = shadow 3 | not listed (0x400/0x800 logged as unknown) |
| 10 | g: "changes the behavior of shadow mode somehow" | (part of sh) | not listed |
| 9 | p: mystery-layer mosaic enable | h: highcolor mosaic enable | p: pivot mosaic enable |
| 8 | s: sprite mosaic enable | s: sprite mosaic enable | s: sprite mosaic enable |
| 7:4 | m: mosaic strength, samples every `16 - m` screen pixels | same | mmmm: 0 = repeat 16, f = every pixel |
| 3:0 | PF3..PF0 mosaic enable | same | enable for PF 4..1 |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
^[raw/docs/taito-f3/website/line-ram.html] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Shadow mode (per line-ram.txt): if two layers are at neighbouring priorities (e.g. 5 and 6), the upper
layer's palette gets its low bit set when the lower layer has a non-transparent pixel at that spot;
the background-colour layer and priority conflicts do not count as opaque. Whether this varies with
bpp is a todo. ^[raw/docs/taito-f3/line-ram.txt] Note: in MAME `fx_6400` (bits 15:10 shifted) is read
but "palette interpretation [unimplemented]". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
FDA-side meaning of the mode/blur bits: [[hardware/fda]]. Mosaic behaviour:
[[hardware/clip-and-mosaic]].

## 6600 (2.3): background colour

`[...b bbbb bbbb bbbb]`: bits 12:0 colour index (palette id 0-8191). ^[raw/docs/taito-f3/line-ram.txt]
^[raw/docs/taito-f3/website/fdp/lineram.html] MAME stores the whole word as `bg_palette` and fills
the line buffer with it at 100% contribution, with the comment "bg palette? [unimplemented]" on the
read. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Layer mix words: 7000, 7200, 7400, B000-B600

The playfield word `[BBEI cccc iiii pppp]` is the model; other layers reuse parts of it. ^[raw/docs/taito-f3/line-ram.txt]

| Field | Bits | Meaning |
|---|---|---|
| B | 15:14 | blend mode (PF: 0 = both, 1 = left (A), 2 = right (B), 3 = neither) |
| E | 13 | enable display |
| I | 12 | invert clip (1 = invert) |
| c | 11:8 | clip plane enables 3..0 |
| i | 7:4 | clip mode per plane: 0 = hide inside, 1 = hide outside |
| p | 3:0 | priority 0-15 (higher = in front) |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/layer.html]
MAME describes bit 13 `E` = enable line, `A` (bit 15) = blend enable and `B` (bit 14) = reversed
blend, `I` as "affects interpretation of inverse mode bits. if on, 1 = invert. if off, 0 = invert";
a layer is drawn only if `(word & 0x2000)` and blend mode != 0b11 (sprites: != 0b00).
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

| Register | Layer | Differences from the model |
|---|---|---|
| B000 / B200 / B400 / B600 (7.0-7.3) | PF1-PF4 | model word |
| 7200 (3.1) | pivot | `[B?EI cccc iiii pppp]`: blend mode is one bit, 0 = both, 1 = left |
| 7000 (3.0) | highcolor ("mystery") layer | `[BBEI cccc iiii pppp]`; highcolor.html: blend mode 0 = both, 1 = A, 2 = B, 3 = neither, "seems to behave differently in FDA mode 2"; line-ram.txt: 0 = both, 1-3 "all seem to be the same half (right?) but with a few pixels that differ"; MAME: unemulated |
| 7400 (3.2) | sprite layer, all groups | `[AAAA ??EI cccc iiii]`: bits 15:12 = group 3..0 blend-value select (bit 12 = group 0); bits 9:0 = enable/invert/clip as in the model shifted down by 4; no priority and no blend mode here |
| 7600 (3.3) | sprite groups | `[dddd cccc bbbb aaaa]`: priority of group 3..0 (0-15) |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
^[raw/docs/taito-f3/website/fdp/highcolor.html] ^[raw/docs/taito-f3/website/fdp/sprite.html]
MAME applies sprite bits 9:0 of 7400 into each group's mix value (`(mix & 0xc00f) | bits<<4`) and bit
`12+group` as the group's blend select. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
The notes also record of blend mode: "note that each layer encodes this setting differently for some reason", and the layer
common settings are priority, enable, 4 clip enables, 4 clip modes, clip invert, blend mode, alpha
select, mosaic enable. ^[raw/docs/taito-f3/website/fdp/layer.html]
Priority resolution and blending live on [[hardware/priority-and-blend]].

## 8000-8600 (4.0-4.3): playfield zoom

`[xxxx xxxx yyyy yyyy]`: bits 15:8 X zoom, 7:0 Y zoom of the playfield the register belongs to, except
Y zoom is swapped between PF1 and PF3.
^[raw/docs/taito-f3/line-ram.txt]

| Register | X zoom (bits 15:8) | Y zoom (bits 7:0) |
|---|---|---|
| 8000 | PF0 (first playfield) | PF0 |
| 8200 | PF1 | PF3 ("BUG") |
| 8400 | PF2 | PF2 |
| 8600 | PF3 | PF1 ("BUG") |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html] (12Me21 numbers the
playfields from 0; MAME's "playfield 2/4" comments are 1-based; its `FIX_Y = {0,3,2,1}` implements the
same swap.) ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] Quirk page:
[[quirks/tilemap-y-zoom-pf1-pf3-swapped]]. Zoom conversion formulas: [[hardware/tilemaps]].

## 9000-9600 (5.0-5.3): playfield palette add

`[.... .... ..cc cccc]` bits 5:0, added to every tile's palette id of PF0..PF3 ("yes it's only 6
bits"). ^[raw/docs/taito-f3/line-ram.txt] MAME multiplies by 16 and adds it to the playfield colour.
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## A000-A600 (6.0-6.3): playfield X scroll adjust

`[ssss ssss ssss ssss]`: same format as the X scroll register: `[iiii iiii iiff ffff]`, negative
fraction. One register per playfield, 0-3. ^[raw/docs/taito-f3/line-ram.txt]
^[raw/docs/taito-f3/website/fdp/tilemap.html] (lineram.html lists three of the four rows with the label
"A000"; the offsets are A000, A200, A400, A600.) ^[raw/docs/taito-f3/website/fdp/lineram.html]
