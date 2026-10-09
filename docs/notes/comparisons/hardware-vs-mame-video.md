---
title: Hardware notes vs MAME 0.289 — video
created: 2026-10-09
updated: 2026-10-09
type: comparison
tags: [video, sprite, tilemap, line-ram, blend, clip, palette, emulator-bug, undocumented]
sources:
  - raw/emu-source/mame-0.289/taito_f3_v.cpp
  - raw/emu-source/mame-0.289/taito_f3.h
  - raw/docs/taito-f3/website/fdp/sprite.html
  - raw/docs/taito-f3/website/fdp/tilemap.html
  - raw/docs/taito-f3/website/fdp/lineram.html
  - raw/docs/taito-f3/website/fdp/clip.html
  - raw/docs/taito-f3/website/fdp/blend.html
  - raw/docs/taito-f3/website/fdp/mosaic.html
  - raw/docs/taito-f3/website/fdp/pivot.html
  - raw/docs/taito-f3/sprite-ram.txt
  - raw/docs/taito-f3/scroll-regs.txt
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/lineram.html, note: "12Me21 line-register layouts from PCB probing and tests"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "MAME 0.289 rewrite (2024) of the F3 video code; its own comments mark many bits unknown or unemulated"}
contradictions:
  - "Sprite 'Enable Scrolls' value 2 (word 2 bits 15..14): notes say it adds global+subglobal; MAME treats bit 15 as 'add nothing'."
  - "Line RAM latches: notes say the latched value persists across frames; MAME rebuilds its latch state at the start of every frame."
  - "Mosaic bit 9 of 0x6400: notes call it the highcolor-layer enable and say the pivot layer has no mosaic; MAME treats it as the pivot-layer enable."
  - "Clip enable bits: notes say 0 = window enabled; MAME treats a set bit as enabled."
  - "Pivot text-tile flip bits: notes put h at bit 15 and v at bit 8; MAME uses bit 15 = flip Y and bit 8 = flip X."
  - "Sprite bank select: notes give two bank bits (4 banks); MAME reads one bit (2 banks)."
  - "Blur field of 0x6400 (bits 13-12): blend.html says 0,1 = disabled and 2,3 = enabled; lineram.html says 0,1 = blurred and 2,3 = normal (the notes disagree with each other; MAME's comment agrees with lineram.html)."
  - "Colour format: notes call fda mode 0 '15-bit rgb'; MAME comments call the legacy mode '12-bit RGB'."
  - "Block control: notes describe the y control as one bit (12) and x as two; MAME reads two bits for each and does nothing for y value 01."
supersedes: []
---

# Hardware notes vs MAME 0.289 — video

This page lines up what 12Me21's notes (`doc`) say about the video hardware against what MAME 0.289
(`emu-source`) implements. A row appears only when both sides speak. MAME ranks higher as evidence,
but neither side is confirmed until a test here; every row is a hypothesis. System-level rows
(timing, interrupts, I/O, clocks) are in [[comparisons/hardware-vs-mame-system]]. Chip pages:
[[hardware/sprites]], [[hardware/tilemaps]], [[hardware/line-ram]], [[hardware/priority-and-blend]],
[[hardware/clip-and-mosaic]], [[hardware/pivot-layer]], [[hardware/fda]]. Format rules: [[SCHEMA]].

Legend for the last column: **agree** = same behaviour on both sides; **differ** = the two sides
disagree; **model** = same data but different description, equivalence not checked.

## Sprites

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Entry layout | 8 words/entry; word 0 = texture low, word 1 = `yzoom:8,xzoom:8`, word 2 = `i:2,s:2,x:12`, word 3 = command bit + y:12, word 4 block/palette, word 6 jump | Same word map (word 0 tile, word 1 zooms, word 2 `iiss xxxx..`, word 3 `c..y`, word 6 jump + 10-bit index) | agree | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L210-L249 (header comment), L1405-L1411] |
| Zoom | scale = 1 − zoom/256 (screen pixels per texture pixel) | `block_scale = 0x100 - zoom` in fixed 8.8 | agree | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1301, L1324] |
| Count limit | ≈880 sprites (128 pixel clocks each, 432×262/128) | Walks up to 0x400 entries; the cap only stops infinite jump loops | differ | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1345] |
| Draw order | later entries are drawn over earlier ones | Draws the list backwards and writes only to empty pixels, so later entries end on top | agree | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1253 (`f3_drawgfx`), L1439 (`draw_sprites`)] |
| Sprite RAM banks | 4 banks of 1024 entries (`$600000`, `$604000`, `$608000`, `$60C000`); most games use 0 and 2; odd banks behave differently (a switch to an odd bank takes effect next frame) | Two banks at word offset 0 and 0x4000 (= bytes `$0` and `$8000`, i.e. the notes' banks 0 and 2); command bit 0 selects; no delay modelled | differ | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1347, L1368] |
| Command word bits | word 5 bits: flipscreen = 13, bpp = 9..8, trails = 1, bank = 10 and 0; bits 12, 7..4 and others have observed but unexplained effects | flipscreen = 13, extra planes = 9..8, trails = 1, bank = bit 0 only; logs "unknown sprite command bits" for the rest | differ (bank) | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/docs/taito-f3/sprite-ram.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L1353-L1368] |
| Trails bit | "supposedly disables clearing framebuffer but seems to be something else" | Skips clearing the sprite framebuffer when set | differ (interpretation) | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1365, L1439 (`draw_sprites`)] |
| Tile number width | word 5 low 2 bits are texture-id upper bits ("might be more than 2"), command or not | `tile = spr[0] | (bit 0 of spr[5] << 16)`: one extra bit; others logged "unknown word 5 bits" | differ | ^[raw/docs/taito-f3/sprite-ram.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L1411] |
| Scroll "set" bits (`s`) | 1 = subglobal, 2 = global, 3 = both; set from the unadjusted position, applied before drawing the same sprite (visible sprite renders at 2× position) | bit 12 sets subglobal, bit 13 sets global, then adds; same 2× effect | agree | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/docs/taito-f3/sprite-ram.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L1303-L1316] |
| Scroll "enable" bits (`i`) | 0 = global+subglobal, 1 = global only, 2 = global+subglobal ("yes, really"), 3 = none | bit 15 set → add nothing; bit 14 set → add global only. So value 2 = none, value 3 = none | differ (value 2) | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/docs/taito-f3/sprite-ram.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L1303-L1316]; see [[quirks/sprite-enable-scrolls-value-2]] |
| Block controls | X control (2 bits): 0 = current pos, 1 = same as previous, 2 = right of previous except first row at current pos, 3 = right of previous. Y control (bit 12; bit 13 "?"): 0 = block origin, 1 = below previous. "Multi" bit 0 stores origin and reads zoom | Same 2-bit decoder for X and Y: 00 = set origin if `!multi` then use origin; 01 = no change; 10 = origin; 11 = advance by 16 × scale | differ (x=2, y=01, and x=0 with multi=1) | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/docs/taito-f3/sprite-ram.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L1318-L1335] |
| Display lag | double-buffered, shown one frame after rendering | Per-game `sprite_lag` 0, 1 or 2 from a table; TODO comment `presumably "sprite lag" is timing of sprite ram/framebuffer access` | model | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L316-L323, L1459-L1470] |
| Groups | four groups by the two top palette bits, each with its own priority, blend mode, alpha select; sprites share one framebuffer so never blend each other | `pri = color bits 7..6`; per-group mix/priority/blend; the sprite framebuffer is shared | agree | ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1432, L795-L818] |

## Tilemaps and pivot

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Tile word | 32 bits: `v,h,tex2:2,bpp:2,as,palette:9,texture:16` | flip YX = word0 bits 15..14, blend select = bit 9, extra planes = bits 11..10, palette 9 bits; tile index = word 1 only | agree except tex2 | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L385-L408] |
| Upper texture bits | `tex2` (bits 29..28) extends the texture id | Ignored ("upper bits of tile number?") | differ | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L385-L408] |
| Extend mode (64×32) | Set by bit 7 of `$66001E`/`$66001F` word | `F3config.extend` per-game table; the register is not consulted | model | ^[raw/docs/taito-f3/scroll-regs.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L316-L361, L1147] |
| Y zoom | `yzoom/128` texture pixels per screen pixel; tilemaps 1 and 3 swapped | `y_scale = low byte << 1` in 8.8, written through `FIX_Y = {0,3,2,1}` | agree | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L823-L827] |
| X zoom | `1 − xzoom/256` texture pixels per screen pixel | `x_scale = 256 − high byte` | agree | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L823-L826] |
| X scroll formula | `Xglobal + Xadjust + 18 + (x + 22 − 4·tnum) · Xzoom` | `(40 − 4·pf)` offset, plus `10 · (x_scale − 256)` described by MAME as "what is with this calculation..." | model (offsets expressed differently) | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L859-L896, L1181] |
| X scroll fraction | ipart 10 bits, fpart 6 bits, "negative fractional part" | `sx ^= 0b11111100` (fraction bits inverted) | agree | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L884] |
| Y scroll accumulation | Accumulator reloaded from the Y global scroll when "Y Scroll Sync" (6000 bit 11) is set (usually line 0); otherwise `+= yzoom` at the start of each line; carries across frames | Accumulator starts every frame from the register and adds `y_scale` after each line except line 0; the sync bit is never read ("is special handling necessary?") | differ (sync bit unused) | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1155, L1242-L1246] |
| Per-line alt tile data and Y adjust | Only tilemaps 2 and 3 (section 0 lacks `4000`/`4200`); `alt` bit 9, `yscroll:9` | Reads only sections 0, subsections 2 and 3: `colscroll` low 9 bits, `alt` bit 9, but alt only when not extend mode | agree (plus MAME's extend restriction) | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L709-L718] |
| Row scroll | `xscroll:16`, same format as the global X scroll | `word << 2`; fraction subtracted rather than added ("allegedly" negative) | agree | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L841-L847] |
| Palette add | 6-bit value added to each tile's palette per line | `pal_add = word × 16`, full 16 bits, added to the colour index | model | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L833-L835] |
| Pivot mode select | Line register `6000`: bit 13 = text/pixel mode, bit 15 = pixel data bank (`$620000` vs `$630000`) | `use_pix()` is true if bit 13 or bit 15 of the word is set (`pivot_control & 0xa0`); bank "unemulated" | differ | ^[raw/docs/taito-f3/website/fdp/pivot.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L733-L735; raw/emu-source/mame-0.289/taito_f3.h L318] |
| Pivot text tile flips | `h,pal:6,v,tex:8`: bit 15 = h-flip, bit 8 = v-flip | text word `[yccc cccx tttt tttt]`: bit 15 = Y flip, bit 8 = X flip | differ | ^[raw/docs/taito-f3/website/fdp/pivot.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L412-L425] |
| Pivot scroll | `$660018` horizontal, `$66001A` vertical, 10 bits | `control_1[4]`/`[5]` used with fixed offsets (−5, −12 flipped) | agree on registers | ^[raw/docs/taito-f3/scroll-regs.txt; raw/emu-source/mame-0.289/taito_f3_v.cpp L1159-L1164] |

## Line RAM

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Section latches | Per scanline per section at `$620000 + section<<9 + scanline<<1`; `l0..l3` bits 3..0 latch, `a0..a3` bits 7..4 add `$800` | Same address and bit use (`latched_addr`): a set bank bit wins over the latch bit | agree | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L694-L707] |
| Section location (`map:4`) | Each section can be remapped to `$620000 + $1000·map`; usually `section + 4` | Fixed at `0x4000 + 0x1000·section` bytes into line RAM; the map nibble is not read | differ | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L701] |
| Latch persistence | Latched value persists across frames (e.g. when a section is 0 on scanline 0) | `f3_line_inf line_data{}` is constructed inside `scanline_draw`, so un-latched fields start at zero each frame | differ | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1147]; see [[quirks/lineram-latch-persists-across-frames]] |
| Lines covered | 256 lines per subsection | 256 lines, reading `255 − y` when flipped | agree | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L1167-L1168] |
| Register `6000` | `s3..s0` blend modes (bits 7..0), `h` bit 8, `p` bit 9 (pivot alpha select), `b` bit 10 (background alpha select), `y` bit 11, `P` bit 13, `B` bit 15 | Sprite blend bits 7..0; pivot blend select bit 9; upper byte kept as `pivot_control`; no background alpha select, no sync bit | partial | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L730-L742] |
| Register `6600` background | `color:13` plus alpha select from `6000` bit 10 | Whole 16-bit word is used as the background palette index at full blend (8/8); comment says "unimplemented" | differ | ^[raw/docs/taito-f3/website/fdp/backgroundcolor.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L774-L780, L1193-L1195] |

## Blend, priority, clip, mosaic, colour

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Alpha value | `(15 − value)/8` clamped to [0,1], four nibbles in `6200` | `min(8, 15 − nibble)` in eighths | agree | ^[raw/docs/taito-f3/website/fdp/alpha.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L746-L751] |
| Blend model | 16 priority rows × 2 columns (half-pixels A and B); each layer drops into rows by priority and columns by blend mode; alpha select picks one of two values per column | Per-pixel src/dst buffers, "contribution" values src/dst chosen by blend mode (normal / reverse / opaque) and blend-select bit | model | ^[raw/docs/taito-f3/website/fdp/priority.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L274-L305, L1002-L1075] |
| Blend-mode encoding | Tilemaps: 0 = both, 1 = A, 2 = B, 3 = none. Sprites: 0 = none, 1 = A, 2 = B, 3 = both | Playfield/pivot: mode 3 disabled, 0 opaque; sprites: 0 disabled, 3 opaque | agree | ^[raw/docs/taito-f3/website/fdp/layer.html; raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L964-L979 and raw/emu-source/mame-0.289/taito_f3.h L270-L272] |
| Pivot blend mode | One bit (bit 15): 0 = A+B, 1 = A | Same two-bit decoder as other layers (bits 15..14) | differ | ^[raw/docs/taito-f3/website/fdp/pivot.html; raw/emu-source/mame-0.289/taito_f3.h L270] |
| Priority conflict | Two layers in one cell mark it invalid; it then shows the background colour (and alpha select if chosen) | Same-priority destination sets `dst_pal = 0`; MAME itself says "prio conflict = color line conflict? (dariusg, bubblem)" and calls it a "HW feature (bug?)" | model | ^[raw/docs/taito-f3/website/fdp/priority.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L296-L304, L1061-L1062] |
| Layer enable bit | Common settings: `E` (bit 13) hidden/displayed | `mix_value & 0x2000` | agree | ^[raw/docs/taito-f3/website/fdp/clip.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L964-L967] |
| Clip window interval | `[left − 47, right − 48)` in screen pixels; `left`/`right` 9 bits (`5000` low byte + upper bit in `4400`/`4600`) | `clip_l = l − 1`, `clip_r = r − 2` in MAME's coordinates (H_START 46); same upper-bit layout | agree | ^[raw/docs/taito-f3/website/fdp/clip.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L711-L728, L914-L915; raw/emu-source/mame-0.289/taito_f3.h L113] |
| Clip enable bits | `cn`: 0 = enabled, 1 = disabled | `clip_enable()` = bits 11..8; set bit = enabled | differ | ^[raw/docs/taito-f3/website/fdp/clip.html; raw/emu-source/mame-0.289/taito_f3.h L273-L275] |
| Clip mode / invert | `in`: 1 = inside visible, 0 = outside; `I` = global invert (bit 12) | `clip_inv()` bits 7..4; bit 12 flips how those bits are read ("if on, 1 = invert") | model | ^[raw/docs/taito-f3/website/fdp/clip.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L898-L908] |
| Mosaic sampling | Sample every `16 − m` pixels; `m` bits 7..4 of `6400`; counter resets 2 pixels before the right edge | `x_sample = 16 − m`; counter based on `x − 46 + 114`, wrapped at 432 ("hw quirk: the counter resets 2 px from the right edge...") | agree | ^[raw/docs/taito-f3/website/fdp/mosaic.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L756-L758, L956-L962] |
| Mosaic enables | Bits 3..0 tilemaps, bit 8 sprites, bit 9 highcolor layer; the pivot layer has none | Bits 3..0 tilemaps, bit 8 sprites, bit 9 pivot layer | differ (bit 9) | ^[raw/docs/taito-f3/website/fdp/mosaic.html; raw/docs/taito-f3/website/fdp/highcolor.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L760-L765] |
| Blur / fda mode bits (`6400` bits 15..12) | `fda:2` (0 = legacy colour, 1 = normal, 2/3 special) and `blur:2` | Header comment "0xf000: palette ram format? [unemulated]", bits `?wBu`: `w` = 12-bit RGB, `B` = 0 enables blur; `fx_6400` only logged | differ (naming); blur numbering inside notes also disagrees | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/docs/taito-f3/website/fdp/blend.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L130-L145, L767-L771] |
| Palette entry format | 12-bit or 24-bit modes chosen by fda mode, per line (notes call mode 0 "legacy color mode (15-bit rgb)") | Per-game switch: SPCINVDX, RIDINGF, ARABIANM, RINGRAGE read the 12-bit `RRRR GGGG BBBB` at bits 15..4; others read 24-bit; comment "TODO … selected on a line basis by 6400?" | differ (per game vs per line; 12 vs 15 bit) | ^[raw/docs/taito-f3/website/fdp/lineram.html; raw/emu-source/mame-0.289/taito_f3_v.cpp L672-L690] |

## Notes-only

- Shadow modes 2 and 3 (palette bit rewrites between neighbouring-priority layers, with a per-bpp table and a layer-pair compatibility matrix) ^[raw/docs/taito-f3/website/fdp/shadow.html]. The 0.289 video code has no shadow handling; see [[hardware/priority-and-blend]].
- Highcolor layer (4×16 textures, 16 bpp, reuses tilemap data, never used by any game) ^[raw/docs/taito-f3/website/fdp/highcolor.html]. MAME does not render it.
- Tilemap control word `$66001F` bit 0 "lock" and bit 1 "blank video" ^[raw/docs/taito-f3/scroll-regs.txt].
- Sprite command bits whose effects depend on which banks hold them (disappearing sprites, garbage pixels, video-signal changes) ^[raw/docs/taito-f3/sprite-ram.txt].

## MAME-only

- Per-game `sprite_lag` (0/2) and `extend` table ^[raw/emu-source/mame-0.289/taito_f3_v.cpp L316-L361].
- Pivot-layer palette hack: the pixel layer reads text RAM palette data and switches to the second half when scrolled past line 256 ^[raw/emu-source/mame-0.289/taito_f3_v.cpp L436-L456].
- Unknown/unemulated bits it logs: `6000` pivot control bits, `7000` "pivot enable", `7400` bits 11..10, sprite word 3/5/6/7 bits ^[raw/emu-source/mame-0.289/taito_f3_v.cpp L733-L818, L1351-L1392].
- Flipscreen coordinate handling (tilemap scroll offsets, sprite mirroring, reversed line RAM index) ^[raw/emu-source/mame-0.289/taito_f3_v.cpp L859-L896, L1414-L1428].

## What this means for a runtime

Rows marked *differ* are the places where copying MAME blindly or following the notes blindly can give
different pictures. Each should be settled by a test (a frame dump from a game that uses the feature)
before the runtime hard-codes either behaviour. Hypothesis: the safest default for the renderer is
MAME's behaviour where a game is known to run correctly under it, and the notes' behaviour for
anything MAME marks unemulated.
