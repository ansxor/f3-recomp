---
title: FDP sprite list, block controls and framebuffer
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, sprite, undocumented]
sources: [raw/docs/taito-f3/website/fdp/sprite.html, raw/docs/taito-f3/sprite-ram.txt, raw/docs/taito-f3/graphics-structs.txt, raw/docs/taito-f3/website/fdp-memory.html, raw/docs/taito-f3/terms.txt, raw/docs/taito-f3/website/fdp/unit-ramble.txt, raw/docs/taito-f3/website/fdp/sprite-tess.svg, raw/docs/taito-f3/website/fdp/sprite-tess2.svg, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: [0x00600000, 0x00604000, 0x00608000, 0x0060C000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/sprite.html, note: "entry format, scroll/block-control semantics, settings register; page itself carries many todos"}
  - {kind: doc, ref: raw/docs/taito-f3/sprite-ram.txt, note: "hardware-test observations of scroll bits, word 5 bits, bank switching"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "get_sprite_info / f3_drawgfx: MAME's model of the same list"}
contradictions:
  - "X block control value 2: sprite.html says 'right of previous sprite, except first row renders at current position'; fdp-memory.html and MAME say 'reset axis' (use the block origin); sprite-ram.txt's pipeline says POS1.x = POS2.x; POS2.x = POS0.x ('increment (broken)')."
  - "Y block control width: sprite.html / graphics-structs.txt use 1 bit (bit 12) with bit 13 unknown; fdp-memory.html and MAME treat bits 13:12 as a 2-bit field like X."
  - "Enable-scroll value 0b10: notes (sprite.html, graphics-structs.txt, sprite-ram.txt) say it behaves like 00 (use both); MAME adds neither scroll for any value with bit 15 set (0b1000, 0b1100, 0b10xx). Per sprite-ram.txt 0b1000 = 'use both', 0b1100 = 'ignore both'; MAME: bit 15 alone already ignores both."
  - "Settings register bits 9:8: sprite.html names them bpp (texture bit planes) and MAME agrees (00=4bpp, 01=5bpp, 11=6bpp); sprite-ram.txt describes the same bits as f/g with garbage-pixel effects and gives bit 10 as 'bank select 2'."
  - "Texture id high bits: MAME takes one extra bit (word 5 bit 0); the notes say at least 2 bits (carts may support 3-4)."
supersedes: []
---

# FDP sprites

Part of the [[hardware/fdp]]. Sprite rendering is separate from line RAM: "sprite rendering itself
is not affected by any lineram settings"; the finished framebuffer is then fed into the layer mixer
as four groups, controlled by [[hardware/line-ram]] and [[hardware/priority-and-blend]].
^[raw/docs/taito-f3/website/fdp/sprite.html]

## Capabilities

- Textures are 16x16 px, up to 64 colours including transparent. ^[raw/docs/taito-f3/website/fdp/sprite.html]
- Maximum about 880 sprites, no per-scanline limit; each entry takes 128 pixel clocks, so
  432 x 262 / 128 = ~884 per frame ("in reality the last few are rendered incorrectly").
  ^[raw/docs/taito-f3/website/fdp/sprite.html] A 1024-entry list (MAME: `0x400`) is the index range.
  ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Sprites can be scaled *down* independently on each axis (not up). ^[raw/docs/taito-f3/website/fdp/sprite.html]
- Double buffered: sprites are displayed one frame after being rendered ("It is assumed").
  ^[raw/docs/taito-f3/website/fdp/sprite.html]
- Draw order: later list entries draw over earlier ones. ^[raw/docs/taito-f3/website/fdp/sprite.html]

## Sprite RAM layout

Sprite RAM is four banks of 1024 entries; each entry is eight 16-bit words (16 bytes), so each
bank is 0x4000 bytes. Address of word `w` of entry `i` in bank `b`:
`0x600000 + b*0x4000 + i*16 + w*2`. ^[raw/docs/taito-f3/website/fdp/sprite.html]
^[raw/docs/taito-f3/graphics-structs.txt] `graphics-structs.txt` writes the address as
`[...00 bbii iiii iiii ----]`: index = bits 13:4, bank = bits 15:14. ^[raw/docs/taito-f3/graphics-structs.txt]
Most games use banks 0 and 2 as their double buffer; the notes are unsure why, and say that the
odd banks behave differently ("bank number is maybe not a correct abstraction"). ^[raw/docs/taito-f3/website/fdp/sprite.html]
MAME only has two lists (offsets 0 and word 0x4000 = byte 0x8000) selected by bit 0 of the command
word, and does not model the other bank bit. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

On each frame the FDP walks the list starting at index 0 of the current bank; commands and jumps
change the current index and bank. ^[raw/docs/taito-f3/website/fdp/sprite.html]

## Entry format

| Word | Bits | Field | Notes |
|---|---|---|---|
| 0 | 15:0 | tile (texture) id, low 16 | ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 1 | 15:8 | Y zoom | scale = 1 - zoom/256 screen px per texture px ^[raw/docs/taito-f3/graphics-structs.txt] |
| 1 | 7:0 | X zoom | same format ^[raw/docs/taito-f3/graphics-structs.txt] |
| 2 | 15:14 | "enable scrolls" (`i`) | which scroll values to add to the position; see below ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 2 | 13:12 | "set scrolls" (`s`) | which scroll values to write from this position ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 2 | 11:0 | X position | signed, 12 bits ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 3 | 15 | command | 1 = command mode, word 5 is written to the settings register ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 3 | 14:12 | unknown | MAME notes games set bits here ("probably just the position overflowing") ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] |
| 3 | 11:0 | Y position | signed, 12 bits ("the x and y positions are 12 bits (e.g. 0x7000 = 0x0000 != 0x0800)") ^[raw/docs/taito-f3/sprite-ram.txt] |
| 4 | 15:14 | X block control | see below ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 13 | unknown (Y block control high bit in fdp-memory.html/MAME) | ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 12 | Y block control | ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 11 | "multi" (M) | affects the *next* sprite; 0 = this sprite's position becomes the block origin and zoom is read; 1 = leave them ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 10 | palette lock (p) | 1 = reuse the previous sprite's palette ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 9 | vertical flip | ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 8 | horizontal flip | ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 4 | 7:0 | palette | palette id = 256 + value (colour index `(0x1000 + value*16)` in MAME's terms); bits 7:6 pick the sprite group ^[raw/docs/taito-f3/website/fdp/sprite.html] ^[raw/docs/taito-f3/terms.txt] |
| 5 | 15:0 | normal: tile id high bits (>= 2 bits, maybe more; bits 1:0 per sprite.html); command: settings, see [[hardware/sprite-command-word]] | ^[raw/docs/taito-f3/sprite-ram.txt] ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 6 | 15 | jump | 1 = next index is `dest`, else current + 1 ^[raw/docs/taito-f3/website/fdp/sprite.html] |
| 6 | 9:0 | jump destination | "sprite jump is not 12 bits ... just 10" ^[raw/docs/taito-f3/sprite-ram.txt] |
| 7 | - | unused | ^[raw/docs/taito-f3/website/fdp/sprite.html] |

MAME decodes the same word 4 as `[bbbb mlyx cccc cccc]` (b = block controls, m = multi, l = palette
lock, y/x = flips) and word 3/word 5 as above, with a single extra tile bit. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Word 4's flip order in MAME (bit 9 = Y flip, bit 8 = X flip) matches the notes. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

A sprite is still rendered when the command bit is set, and when the low 16 bits of the id are 0
with upper bits set; whether an all-zero id is drawn is unknown ("our rom has a blank sprite
there"). ^[raw/docs/taito-f3/sprite-ram.txt] MAME skips entries whose full 17-bit tile id is 0.
^[raw/emu-source/mame-0.289/taito_f3_v.cpp] An entry can be a visible sprite and/or a command and/or
a scroll-set; games usually do one at a time. ^[raw/docs/taito-f3/terms.txt]

## Zoom

Screen pixels per texture pixel = `1 - zoom/256`; examples: `$00` 16 px, `$40` 12 px, `$80` 8 px,
`$C0` 4 px, `$FF` 0.0625 px (sprite width on screen). ^[raw/docs/taito-f3/website/fdp/sprite.html]
Observed oddity: one axis appears to be positioned with sub-pixel precision and the other not, and
they downscale differently; the notes do not say which is which. ^[raw/docs/taito-f3/website/fdp/sprite.html]
MAME treats the scale as `0x100 - zoom` in 8-bit fixed point and steps 16 source pixels through it
for both axes, rounding Y up by 255/256 when not flipped. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Scroll values: global and subglobal

Two scroll values per frame ("global" and "subglobal"; `graphics-structs.txt` says "local" and
"global"). Each sprite entry can write them from its own position and choose which to add.
^[raw/docs/taito-f3/terms.txt] ^[raw/docs/taito-f3/graphics-structs.txt]

Set bits (word 2 bits 13:12): ^[raw/docs/taito-f3/website/fdp/sprite.html]

| Value | Effect |
|---|---|
| 00 | set neither |
| 01 | write subglobal ("local") |
| 10 | write global |
| 11 | write both |

Enable bits (word 2 bits 15:14), per sprite.html/graphics-structs.txt: ^[raw/docs/taito-f3/website/fdp/sprite.html]

| Value | Effect |
|---|---|
| 00 | add global and subglobal |
| 01 | add global only |
| 10 | add global and subglobal ("yes, really") |
| 11 | add neither |

In MAME the four bits are `[ignore-all, ignore-subglobal, set-global, set-subglobal]` read from
word 2 bits 15:12; bit 15 set adds nothing, otherwise global is added and subglobal is added unless
bit 14 is set. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] (This differs from the notes for `10xx`;
see contradictions and [[quirks/sprite-enable-scrolls-value-2]].)

Hardware observations (`sprite-ram.txt`): ^[raw/docs/taito-f3/sprite-ram.txt]

- Both scrolls can be set in one entry (0b1111 sets both to the same value). ^[raw/docs/taito-f3/sprite-ram.txt]
- Scroll is always set from the *un-adjusted* position; the ignore bits do not affect the value
  written. ^[raw/docs/taito-f3/sprite-ram.txt]
- Scroll is applied *before* rendering the current sprite, so setting scroll on a visible sprite
  renders it at twice the listed position (see [[quirks/sprite-set-scroll-affects-same-sprite]]). ^[raw/docs/taito-f3/sprite-ram.txt]
- Scroll values are 12-bit and persist until rewritten (MAME keeps them per axis across the frame,
  starting from 0). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Processing order and block (tessellation) controls

Order of operations for one entry, per `sprite.html`: read position; process set-scrolls; process
enable-scrolls; process multi (set block origin, read zoom); process X and Y block control;
read palette/texture/flip; render; save left/right/bottom edges; process jump; process commands (it
is unknown whether commands apply at the end or mid-entry). ^[raw/docs/taito-f3/website/fdp/sprite.html]
`sprite-ram.txt` gives the same flow with named positions: POS0 is the entry position (after
scrolls), POS1 the previous sprite's position, POS2 the render position; `POS1.y = POS0.y` only if
the *old* Y-lock/multi is 0; `POS2.y = POS1.y` when Y mode is 0; X mode 0: `POS2.x = POS1.x = POS0.x`,
1: `POS2.x = POS1.x`, 2: `POS1.x = POS2.x; POS2.x = POS0.x`, 3: `POS1.x = POS2.x`; the sprite is
rendered at POS2 (resetting POS2.x from POS1.x after each row). ^[raw/docs/taito-f3/sprite-ram.txt]

Block control values as documented by each source (see the contradictions field):

| Value | `sprite.html` X ctrl | `fdp-memory.html` X/Y ctrl | MAME X/Y ctrl |
|---|---|---|---|
| 0 / 00 | X = current position (Y: Y block origin; if multi was 0 this is the current position) | set as origin and reset axis | if previous entry's multi was 0: store position as block origin and zoom as block scale; then pos = block origin |
| 1 / 01 | same as previous sprite | "?" | pos unchanged |
| 2 / 10 | right of previous sprite, first row at current position | reset axis | pos = block origin |
| 3 / 11 | right of previous sprite | increment axis | pos += block scale x 16 |

Sources: ^[raw/docs/taito-f3/website/fdp/sprite.html] ^[raw/docs/taito-f3/website/fdp-memory.html]
^[raw/emu-source/mame-0.289/taito_f3_v.cpp] For Y, `sprite.html` documents only two values: 0 = Y block
origin, 1 = below the previous sprite. ^[raw/docs/taito-f3/website/fdp/sprite.html]

Blocks are column-major: there is no way to place a sprite directly to the right of the previous
one. Rectangular blocks use `(0,0)` for the first sprite (with multi = 0 on the *previous* entry),
`(3,0)` for the first sprite of each later column, and `(1,1)` for the rest. ^[raw/docs/taito-f3/website/fdp/sprite.html]
(Pairs are (X ctrl, Y ctrl).) A typical 3x3 block is drawn in `raw/docs/taito-f3/website/fdp/sprite-tess.svg`
and `sprite-tess2.svg`; only the first sprite has a position in RAM. ^[raw/docs/taito-f3/website/fdp/sprite.html]
Per `graphics-structs.txt`, Y-lock "multi" affects the *following* sprite: 1 = do not update block
origin and zoom. ^[raw/docs/taito-f3/graphics-structs.txt]

## Command word

Word 5 of an entry whose word 3 bit 15 is set is written to a persistent settings register: flipscreen,
texture bit planes, framebuffer clearing, sprite-RAM bank select and a few unknown bits. Full bit
table, bank-switching observations and MAME's decoding: [[hardware/sprite-command-word]].
Bank behaviour is also summarised in [[quirks/sprite-odd-bank-lasts-one-frame]].
^[raw/docs/taito-f3/website/fdp/sprite.html] ^[raw/docs/taito-f3/sprite-ram.txt]

## Sprite groups and the framebuffer

Sprites are rendered into one 12bpp framebuffer (pixel = palette id << 4 | texture colour).
^[raw/docs/taito-f3/graphics.txt] The group is the top 2 bits of the 8-bit palette
(`0x00, 0x40, 0x80, 0xC0`); each group has its own blend mode (`6000`), blend-value select (`7400`)
and priority (`7600`), while enable/clip (`7400`) and mosaic enable (`6400`) are shared. Because the
buffer is shared, sprites of different groups cannot overlap or blend with one another.
^[raw/docs/taito-f3/terms.txt] ^[raw/docs/taito-f3/graphics.txt] ^[raw/docs/taito-f3/website/fdp/sprite.html]
Register bit tables: [[hardware/line-ram]]. MAME adds that sprites are rasterised into the buffer only
where the destination pixel is still empty, iterating the list in reverse, and keeps per-row "group
used" flags for the mixer. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Timing model in MAME

`screen_update` runs sprite parse/draw before, after or between the scanline mixer depending on a
per-game "sprite lag" (0, 1 or 2) table; the source calls it a guess about FDP RAM/framebuffer
access timing. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] The runtime therefore should not assume a
single lag value for all games. See also [[hardware/fdp]].
