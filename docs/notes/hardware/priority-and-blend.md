---
title: Layer priority, blending, alpha select and background color
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, blend, line-ram, palette, sprite, tilemap]
sources: [raw/docs/taito-f3/website/fdp/priority.html, raw/docs/taito-f3/website/fdp/alpha.html, raw/docs/taito-f3/website/fdp/blend.html, raw/docs/taito-f3/website/fdp/backgroundcolor.html, raw/docs/taito-f3/website/fdp/layer.html, raw/docs/taito-f3/terms.txt, raw/docs/taito-f3/graphics.txt, raw/docs/taito-f3/graphics-structs.txt, raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/website/fdp/lineram.html, raw/emu-source/mame-0.289/taito_f3_v.cpp, raw/emu-source/mame-0.289/taito_f3.h]
games: []
addresses: [0x00620000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/priority.html, note: "priority table / half-pixel model (a theory fitted to hardware tests)"}
  - {kind: doc, ref: raw/docs/taito-f3/line-ram.txt, note: "per-layer mix words and blend-select bits"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "src/dst two-contribution model in mix_line and render_line"}
contradictions:
  - "Half-pixel naming vs register nibbles: website alpha.html puts A0/A1 in bits 3:0 and B0/B1 in bits 7:4 / 15:12 ('confirm the order' todo); line-ram.txt labels the low byte 'right' and the high byte 'left'; terms.txt says 'A and B' = 'left and right'. Whether A is the left or the right half-pixel is unresolved."
  - "Blend mode numbering: line-ram.txt gives playfield/highcolor-capable layers 0=both,1=left,2=right,3=neither but sprite groups 0=neither,1=left,2=right,3=both; for highcolor (7000) the website says 0 both/1 A/2 B/3 neither while line-ram.txt says 1,3='some half, ???'."
  - "MAME maps field value 01 (bit 14) to the second pair of 6200 values and 10 (bit 15) to the first pair ('normal'/'reverse' blend), and treats equal blend modes as non-blending; 12Me21's priority-cell model uses value-selected half-pixel columns instead."
  - "Priority conflict: 12Me21 says a conflicted cell yields the background color; MAME writes palette index 0 for the destination pixel."
  - "Background color: 12Me21 gives it its own alpha select (2.0 bit 10); MAME fills background with 100% contribution and does not use the bit."
supersedes: []
---

# Layer priority, blending, alpha select and background color

This page describes how the FDP chooses the two colors it hands to the [[hardware/fda]] for each screen
pixel. All of this is per-scanline state held in [[hardware/line-ram]]. Clip bits are in
[[hardware/clip-and-mosaic]]; shadow in [[hardware/shadow-mode]]; the extra layer in
[[hardware/highcolor-layer]].

## Layers and their per-scanline mix data

There are eight graphics layers plus the background color: pivot/text (1), sprite groups (4), tilemaps
(4), plus the hidden highcolor layer, which shares tilemap data. ^[raw/docs/taito-f3/graphics.txt]
For each screen pixel every layer supplies: ^[raw/docs/taito-f3/website/fdp/priority.html]

- Color Id (13-bit index into color RAM), ^[raw/docs/taito-f3/website/fdp/priority.html]
- Alpha Select (1 bit), ^[raw/docs/taito-f3/website/fdp/priority.html]
- Enable (0 if transparent, clipped, or disabled), ^[raw/docs/taito-f3/website/fdp/priority.html]
- Priority Value (4 bits), ^[raw/docs/taito-f3/website/fdp/priority.html]
- Blend Mode (2 bits). ^[raw/docs/taito-f3/website/fdp/priority.html]

Most layers share one 16-bit mix word `bm:2, E, I, c3..c0, i3..i0, prio:4` (see the clip page for the
clip bits; E is layer enable, bits 15–14 the blend mode, bits 3–0 the priority). ^[raw/docs/taito-f3/website/fdp/lineram.html]
^[raw/docs/taito-f3/line-ram.txt]

| Layer | Mix word | Notes | Source |
|---|---|---|---|
| Tilemap 0–3 | 7.0–7.3 ("B000"–"B600") | blend select is per tile (bit 25 of the tile data word, `v` in the tile struct) | ^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/graphics-structs.txt] |
| Pivot (text/pixel) | 3.1 ("7200") | `bm` is one bit: 0 = both, 1 = left; "different interpretation than elsewhere"; blend select from 2.0 bit 9 | ^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html] |
| Sprite groups 0–3 | clip: 3.2 ("7400"); priority: 3.3 ("7600") one nibble per group; blend mode: 2.0 ("6000") two bits per group | enable, clip and mosaic enable are shared between groups | ^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/graphics.txt] |
| Highcolor | 3.0 ("7000"), see [[hardware/highcolor-layer]] | | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |
| Background | 2.3 ("6600") color, 2.0 bit 10 alpha select | priority hardcoded to 0, always enabled, cannot be clipped | ^[raw/docs/taito-f3/graphics.txt] ^[raw/docs/taito-f3/website/fdp/backgroundcolor.html] |

Sprite groups are chosen by the upper two palette bits of the sprite (00, 40, 80, C0); each group has
its own blend settings, but the groups share one framebuffer so they cannot overlap or blend with each
other. Sprite priority (draw order within the framebuffer) is distinct from FDP priority and is mapped
to it through line register 3.3 ("7600"). ^[raw/docs/taito-f3/terms.txt] ^[raw/docs/taito-f3/graphics.txt]
MAME's group selector test is `BIT(color, 10, 2) != group` on the framebuffer value. ^[raw/emu-source/mame-0.289/taito_f3.h]

## The priority cell array (12Me21's theory)

"Only a theory, but so far it's been useful": each layer's color is placed into a table of 16 rows
(priority) × 2 columns (half-pixels A and B); each cell holds a 13-bit color id, a transparency flag,
an alpha-select bit and maybe an "invalid" flag (16 bits total). ^[raw/docs/taito-f3/terms.txt]

1. The priority value selects the row; the blend mode selects column A, B, both, or neither (if Enable
   is set). A layer can fill both columns or none, but always exactly one row. ^[raw/docs/taito-f3/website/fdp/priority.html]
2. If two layers write the same cell it is marked invalid and contributes the background color if
   chosen; "this means the order that the layers are processed in doesn't matter". ^[raw/docs/taito-f3/website/fdp/priority.html]
3. For each column, the highest non-empty cell is sent on (color id + alpha) to the FDA. Higher
   priority values are in front. ^[raw/docs/taito-f3/website/fdp/priority.html]
4. If nothing is found the background color is used; a todo in the notes leaves open whether the
   "background layer always fills priority row 0" or "row 0 doesnt exist and that bg color is just a fallback value". ^[raw/docs/taito-f3/website/fdp/priority.html]
5. The alpha for each chosen cell is looked up from its column and alpha select (next section). ^[raw/docs/taito-f3/website/fdp/priority.html]
6. Shadow mode is special circuitry between neighbouring rows ([[hardware/shadow-mode]]). ^[raw/docs/taito-f3/website/fdp/priority.html]

Per-line conflict behaviour is covered by [[quirks/priority-conflict-shows-background]].

### Blend mode values

| Layer type | 0 | 1 | 2 | 3 | Source |
|---|---|---|---|---|---|
| Playfield | both | left | right | neither | ^[raw/docs/taito-f3/line-ram.txt] |
| Sprite group | neither | left | right | both | ^[raw/docs/taito-f3/line-ram.txt] |
| Pivot (1 bit) | both | left | – | – | ^[raw/docs/taito-f3/line-ram.txt] |
| Highcolor (website) | both | A | B | neither | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |
| Highcolor (line-ram.txt) | both | ??? | ??? | ??? ("they all seem to be the same half (right?) but with a few pixels that differ") | ^[raw/docs/taito-f3/line-ram.txt] |

"Left/right" and "A/B" are used as synonyms in the glossary. ^[raw/docs/taito-f3/terms.txt] The generic
layer page lists blend mode as "which half-pixel(s) to contribute to (neither/A/B/both)" and adds that
"each layer encodes this setting differently for some reason". ^[raw/docs/taito-f3/website/fdp/layer.html]

## Alpha values and alpha select

Line register 2.1 ("6200") holds four 4-bit alpha values, two for each half-pixel; each layer's alpha
select bit chooses one of the two in its column. ^[raw/docs/taito-f3/website/fdp/alpha.html]

- Website layout: `B1:4, B0:4, A1:4, A0:4` (MSB first; "confirm the order of these"). ^[raw/docs/taito-f3/website/fdp/alpha.html]
- line-ram.txt layout: `[LLLL llll RRRR rrrr]` — r/R = "right" pixel, select 0/1; l/L = "left" pixel. ^[raw/docs/taito-f3/line-ram.txt]
- Formula: `alpha = clamp((15 − value)/8, 0, 1)`. $0–$7 = 100 %, $8 = 87.5 %, $9 = 75 %, $A = 62.5 %,
  $B = 50 %, $C = 37.5 %, $D = 25 %, $E = 12.5 %, $F = 0 %. ^[raw/docs/taito-f3/website/fdp/alpha.html]
- The glossary describes alpha value as "ranges from $7 (100%) to $F (0%)". ^[raw/docs/taito-f3/terms.txt]

Where the alpha select bit comes from: ^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]

| Layer | Alpha select source |
|---|---|
| Tilemap | per tile (tile word bit 25) |
| Sprite group n | 3.2 ("7400") bits 15–12 (bit 12+n) |
| Pivot | 2.0 ("6000") bit 9 |
| Highcolor | 2.0 bit 8 |
| Background color | 2.0 bit 10 |

## Background color

A pseudo-layer used when nothing else contributes to a half-pixel (or a priority conflict occurs);
priority hardcoded to 0, almost no settings. ^[raw/docs/taito-f3/website/fdp/backgroundcolor.html]

- 2.3 ("6600"): low 13 bits = color id (0–8191); top 3 bits ignored (`#:3, color:13`). ^[raw/docs/taito-f3/website/fdp/backgroundcolor.html]
  line-ram.txt: "seems to be all bg color index but not 100% sure". ^[raw/docs/taito-f3/line-ram.txt]
- 2.0 ("6000") bit 10: alpha select. ^[raw/docs/taito-f3/website/fdp/backgroundcolor.html]
- MAME copies the whole 16-bit 6600 word into the background palette index (comment: "always palette 0 in
  existing games") and fills the background with 100 % contribution. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] ^[raw/emu-source/mame-0.289/taito_f3.h]

## MAME's equivalent model

MAME works with two contributions rather than 16×2 cells. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

- Layers are drawn per scanline, sorted by priority (stable, highest first); layer order on ties is
  fixed as pivot, sprite0, pf0, sprite3, pf3, sprite2, pf2, sprite1, pf1 ("seems ok for dariusg/gseeker/bubblem conflicts?"). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Each layer has a blend_a bit (0x4000) and blend_b bit (0x8000). Playfield/pivot layers are disabled when
  both set (value 3); sprite groups are disabled when both are zero. Opaque layers (both blend bits 0 or
  both 1 depending on type) use both contribution values. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- A layer is skipped for a pixel if its blend mode equals the blend mode of the source already stored
  there ("layers cannot blend against the same blend mode"). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Source contributions come from `blend[sel]` (value 10) or `blend[2+sel]` (value 01), with `blend[0..3]` =
  6200 nibbles from low to high; opaque layers set src = `blend[2+sel]` and dst = `blend[sel]`. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- MAME header comment notes only up to two layers should contribute to a pixel and that a priority
  conflict "seems like the second layer can reset part of the state (dariusg stage V' clouds)". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Palette add (per-line, tilemaps) is added to the palette index before blending: `pal + pal_add*16`. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Hypothesis: MAME's source/destination roles and 12Me21's A/B columns describe the same two-half-pixel
hardware; whether MAME's "normal blend" is the left or right half-pixel cannot be determined from the
sources alone.
