---
title: Highcolor layer (hidden FDP layer)
created: 2026-10-09
updated: 2026-11-18
type: hardware
tags: [video, tilemap, blend, undocumented]
sources: [raw/docs/taito-f3/website/fdp/highcolor.html, raw/docs/taito-f3/graphics.txt, raw/docs/taito-f3/terms.txt, raw/docs/taito-f3/line-ram.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp, raw/docs/fdp-7000-and-sprite-command-12-remark-2026-11-18.txt]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/fdp-7000-and-sprite-command-12-remark-2026-11-18.txt, note: "12 (2026-11-18): 7000 is the highcolor layer's blend controls, same format as B000; MAME does not emulate the layer because no game uses it"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/highcolor.html, note: "tested by the author on hardware; no game is known to use it"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "MAME only decodes 7000/6400 bits as unemulated pivot/mosaic bits; no highcolor rendering"}
contradictions:
  - "Blend mode of line register 3.0: website gives 0 both, 1 A, 2 B, 3 neither; line-ram.txt says 0 = both and 1,2,3 = '??? half something, same half (right?) with a few pixels that differ'."
  - "Line register 2.2 bit 9: 12Me21 = highcolor mosaic enable; MAME = pivot mosaic enable."
  - "MAME treats register 7000 as a 'pivot enable' word (unemulated); 12 confirmed on 2026-11-18 that it is the highcolor layer's blend/mix word in the B000 format. MAME's name is wrong."
supersedes: []
---

# Highcolor layer (hidden FDP layer)

A special layer that reuses the data from the four tilemap layers. It can be used at the same time as the
tilemaps and even blend with them, "but it is not useful to do so". ^[raw/docs/taito-f3/website/fdp/highcolor.html]
Also called "mystery layer"/"hicolor/picture layer (tentative name)" in the notes. ^[raw/docs/taito-f3/terms.txt] ^[raw/docs/taito-f3/graphics.txt]

- Effectively a 32×32 (64×32 in extend mode) tile layer where each tile is a 4×16 ROM texture with up to
  255(?) colors plus transparency; in theory 16 bits per pixel. It cannot be scrolled or scaled
  horizontally. ^[raw/docs/taito-f3/website/fdp/highcolor.html]
- graphics.txt describes it as a 128×32 (or 256×32) grid of 4×16 tiles at 16 bpp, "though only 13 can be
  used in practice" (the FDP color id is 13 bits). ^[raw/docs/taito-f3/graphics.txt]
- "As far as we know, no games have used this feature", but the author tested it enough to know it works.
  How the texture data is sourced is not yet written up. ^[raw/docs/taito-f3/website/fdp/highcolor.html]

## Registers

| Register | Field | Source |
|---|---|---|
| 2.0 "6000" bit 8 | alpha select | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |
| 2.2 "6400" bit 9 | mosaic enable | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |
| 3.0 "7000" bits 3–0 | priority (0–15) | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |
| 3.0 "7000" bits 13–4 | clip settings (`E I cccc iiii`, see [[hardware/clip-and-mosaic]]) | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |
| 3.0 "7000" bits 15–14 | blend mode: 0 both, 1 A, 2 B, 3 neither (seems to behave differently in FDA mode 2; untested) | ^[raw/docs/taito-f3/website/fdp/highcolor.html] |

The 12Me21 line-ram.txt calls the blend-mode meaning for 1–3 uncertain. ^[raw/docs/taito-f3/line-ram.txt]
MAME stores the whole 7000 word as `pivot_enable` and logs any non-zero value as unknown; games observed with
non-zero 7000 words include ridingf, commandw, trstar (c000), recalh (4000), dariusg (0001 on some lines). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
12 confirmed (2026-11-18) that 7000 is this layer's blend controls in the same format as the B000 tilemap words,
and that MAME skips the layer because no game uses it. ^[raw/docs/fdp-7000-and-sprite-command-12-remark-2026-11-18.txt]
Neither of our renderers draws this layer or reads 7000 beyond storing it.
Hypothesis: some games write 7000 words (e.g. 0xC000) that, by 12Me21's layout, would set the highcolor blend
mode/priority while leaving it with no texture data to draw; this is not confirmed anywhere.

Related: shadow behaviour onto/from this layer is untested ([[hardware/shadow-mode]]); blend/priority rules
are on [[hardware/priority-and-blend]]; tilemap data it borrows is on [[hardware/tilemaps]].
