---
title: TC0630FDP (F3 Display Processor) overview
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, pcb, memory-map, line-ram, undocumented]
sources: [raw/docs/taito-f3/fdp.txt, raw/docs/taito-f3/website/fdp.html, raw/docs/taito-f3/website/fdp-memory.html, raw/docs/taito-f3/graphics.txt, raw/docs/taito-f3/graphics-structs.txt, raw/docs/taito-f3/graphics-ram-chips.txt, raw/docs/taito-f3/scroll-regs.txt, raw/docs/taito-f3/terms.txt, raw/docs/taito-f3/website/fdp/home.html, raw/docs/taito-f3/website/fdp/layer.html, raw/docs/taito-f3/website/fdp/highcolor.html, raw/docs/taito-f3/website/video.html, raw/docs/taito-f3/draw.txt, raw/emu-source/mame-0.289/taito_f3.cpp, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: [0x00600000, 0x00610000, 0x0061C000, 0x0061E000, 0x00620000, 0x00630000, 0x00660000, 0x0066001E]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp-memory.html, note: "graphics RAM map and sprite/tile/text/latch bit tables (12Me21, WIP page)"}
  - {kind: doc, ref: raw/docs/taito-f3/scroll-regs.txt, note: "write-only scroll/settings registers at 0x660000"}
  - {kind: doc, ref: raw/docs/taito-f3/graphics.txt, note: "layer list and per-scanline parameters"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "main-CPU address map for the FDP RAM windows"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "register comments (control_0/control_1) and tile/text/pixel decode"}
contradictions:
  - "Tile-word flip bits: the notes put horizontal flip at bit 15 and vertical flip at bit 14 of the first tile word (graphics-structs.txt, fdp-memory.html); MAME passes bits 15:14 straight to TILE_FLIPYX, which makes bit 15 vertical and bit 14 horizontal (taito_f3_v.cpp get_tile_info). See [[hardware/tilemaps]]."
  - "Alternate tilemaps in extend mode: the notes' extend-mode address has a 3-bit tilemap number covering the alternate maps; MAME ignores line-RAM alternate selection when extend mode is on (taito_f3_v.cpp read_line_ram, `!m_extend && ...`)."
supersedes: []
---

# TC0630FDP — "F3 Display Processor"

The FDP is the F3's only graphics chip (IC13, 208-pin). 12Me21's notes: it does all graphics
rendering except the final colour lookup and alpha blend; for every screen pixel it outputs two
colour indexes (13 bits each, plus alpha) that address colour RAM, and the [[hardware/fda]]
blends the two looked-up RGB colours into one output pixel.
^[raw/docs/taito-f3/website/fdp.html] ^[raw/docs/taito-f3/website/fdp/home.html]

The chip is steered entirely by writing its external RAM (CPU-visible "graphics RAM") plus a few
write-only internal registers; there is no command FIFO. ^[raw/docs/taito-f3/website/fdp/home.html]
Everything except tile data, global scroll, extend mode, some video-signal settings, sprite data and
textures is a *per-scanline* setting held in [[hardware/line-ram]]. ^[raw/docs/taito-f3/website/fdp/lineram.html]

Related pages: [[hardware/sprites]], [[hardware/tilemaps]], [[hardware/pivot-layer]],
[[hardware/line-ram]], [[hardware/priority-and-blend]], [[hardware/clip-and-mosaic]],
[[hardware/memory-map]], [[hardware/video-timing]].

## Layers

`terms.txt` counts eight layers that can contribute to a pixel: background colour, pivot
(text/pixel), sprites, four tilemaps, and the "highcolor" layer. ^[raw/docs/taito-f3/terms.txt]
(The sprite layer is further split into four *groups* with separate blend/priority settings.) ^[raw/docs/taito-f3/graphics.txt]

| Layer | What it is | Data source | Page |
|---|---|---|---|
| background colour | one solid colour, priority hard-wired to 0, always enabled, cannot be clipped | line RAM `6600` | [[hardware/line-ram]] ^[raw/docs/taito-f3/graphics.txt] |
| pivot | 64x64 grid of 8x8 tiles, text or pixel mode | text RAM + font/pixel RAM | [[hardware/pivot-layer]] ^[raw/docs/taito-f3/graphics.txt] |
| sprites (x4 groups) | rendered from one 12bpp framebuffer; groups chosen by top 2 bits of palette | sprite list RAM | [[hardware/sprites]] ^[raw/docs/taito-f3/graphics.txt] |
| playfields PF1-PF4 | 32x32 (or 64x32) grid of 16x16 tiles, 4/5/6bpp from ROM | playfield RAM | [[hardware/tilemaps]] ^[raw/docs/taito-f3/graphics.txt] |
| highcolor ("mystery") | 128x32 (or 256x32) grid of 4x16 tiles, 16bpp (about 13 usable), reuses the playfield tile ROM data | ROM | below ^[raw/docs/taito-f3/graphics.txt] |

Per-layer, per-scanline parameters (all in line RAM): enable, clipping (global invert + 4 plane
enables + 4 plane inverts), priority 0-15, blend enable / reverse / value-select, mosaic enable.
Mosaic is not supported by the pivot layer per `graphics.txt`, but see the contradiction noted in
[[hardware/line-ram]]. Enable, clipping and mosaic enable are shared across the four sprite groups.
^[raw/docs/taito-f3/graphics.txt]

Pixel pipeline order as given by the notes: per-layer render, mosaic, clipping, priority sort (two
winners), shadow effect, alpha lookup, colour RAM lookup, FDA blend. ^[raw/docs/taito-f3/website/fdp/home.html]

### Highcolor layer

Reuses the data of the four playfields (can be used at the same time, even blended with them);
cannot be scrolled or scaled horizontally; 4x16 textures with "255(?)" colours or in theory 16 bits
per pixel. 12Me21 says "as far as we know, no games have used this feature" but that it works.
Its line-RAM controls are `6000` bit 8 (alpha select), `6400` (mosaic enable, see conflicting bit
positions in [[hardware/line-ram-registers]]; layer page: [[hardware/highcolor-layer]]) and `7000` (priority/clip/blend). MAME does not emulate it
(`7000` is read into `pivot_enable` and never used). ^[raw/docs/taito-f3/website/fdp/highcolor.html]
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Graphics RAM map

Two RAM chips (IC11, IC12, LH5P8128-80, "graphics ram 1" and "graphics ram 2") hang directly off the FDP
and are the CPU-visible store for tilemaps, the sprite list, text/pixel data and line effects.
^[raw/docs/taito-f3/draw.txt] ^[raw/docs/taito-f3/website/fdp.html]
12Me21's map uses the high byte of the offset (`*10...` = `0x610000`); MAME maps the same windows.
^[raw/docs/taito-f3/website/fdp-memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp]

| Main-CPU bus range | Contents | Notes |
|---|---|---|
| 0x00600000-0x0060FFFF | sprite list RAM, 4 banks x 1024 entries x 8 words | the notes' map draws list 0 at +0x0000 and list 1 at +0x8000 (0x4000 bytes each); +0x4000 and +0xC000 are reached through the sprite command word (sprite-ram.txt: "presumably" for 0x60C000), see [[hardware/sprites]] ^[raw/docs/taito-f3/website/fdp-memory.html] ^[raw/docs/taito-f3/sprite-ram.txt] |
| 0x00610000-0x0061BFFF | playfield tile RAM | normal mode: 8 maps x 32x32 longs (0x1000 each); extend mode: 6 maps x 64x32 longs (0x2000 each), see [[hardware/tilemaps]] ^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/website/fdp-memory.html] |
| 0x0061C000-0x0061DFFF | pivot text RAM | 64x64 words ^[raw/docs/taito-f3/website/fdp-memory.html] |
| 0x0061E000-0x0061FFFF | pivot character font | 256 chars x 8 rows x 32 bit (4bpp) ^[raw/docs/taito-f3/website/fdp-memory.html] |
| 0x00620000-0x0062FFFF | line RAM: latch table at 0x620000, "pivot port" at 0x621000, then 14 blocks of 0x1000 (0x622000-0x62FFFF) | see [[hardware/line-ram]] ^[raw/docs/taito-f3/website/fdp-memory.html] |
| 0x00630000-0x0063FFFF | pivot pixel data (when bank bit selects 0x630000) | 64x32 cells x 8 rows of longs; the alternate bank location is 0x620000, which collides with line RAM ^[raw/docs/taito-f3/website/fdp/pivot.html] |
| 0x00660000-0x0066001F | FDP write-only registers | see below ^[raw/docs/taito-f3/scroll-regs.txt] |

MAME's windows: `0x600000-0x60ffff` sprite RAM, `0x610000-0x61bfff` playfield RAM,
`0x61c000-0x61dfff` text RAM, `0x61e000-0x61ffff` char RAM, `0x620000-0x62ffff` line RAM,
`0x630000-0x63ffff` pivot RAM, `0x660000-0x66000f` / `0x660010-0x66001f` control_0 / control_1.
^[raw/emu-source/mame-0.289/taito_f3.cpp] MAME's `taito_f3.h` sizes: sprite RAM 0x10000, text RAM
0x2000, line RAM 0x10000, pivot RAM 0x10000 bytes. ^[raw/emu-source/mame-0.289/taito_f3.h]
The full CPU address map (work RAM, palette, sound, I/O) is on [[hardware/memory-map]].

CPU-side address decoding as 12Me21 writes it (`*` = any prefix bits) is repeated on each layer's
page; the generic layout is `[...01 xxxx ...]` for tile data and `[*010 ...]` for line RAM.
^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/line-ram.txt]

## Write-only registers (0x660000)

Scroll and settings. MAME splits them into `control_0` (words 0-7) and `control_1` (words 8-15).
^[raw/emu-source/mame-0.289/taito_f3.cpp]

| Address | Meaning | Data format | Source |
|---|---|---|---|
| 0x00660000 + 2n (n = 0..3) | playfield n horizontal scroll | `[iiii iiii iiff ffff]`: integer part bits 15:6, "negative fractional part" bits 5:0 | ^[raw/docs/taito-f3/scroll-regs.txt] |
| 0x00660008 + 2n (n = 0..3) | playfield n vertical scroll | tilemap.html: `ipart:9, fpart:7` (integer 0-511, fraction in 1/128; sign of fraction unknown: "is this negative like x scroll?"); scroll-regs.txt says "(forgot)" | ^[raw/docs/taito-f3/website/fdp/tilemap.html] ^[raw/docs/taito-f3/scroll-regs.txt] |
| 0x00660010-0x00660017, 0x0066001C-0x0066001D | "afaik ... do nothing" | | ^[raw/docs/taito-f3/scroll-regs.txt] |
| 0x00660018 | pivot horizontal scroll | `[.... ..ss ssss ssss]` | ^[raw/docs/taito-f3/scroll-regs.txt] |
| 0x0066001A | pivot vertical scroll | `[.... ..ss ssss ssss]` | ^[raw/docs/taito-f3/scroll-regs.txt] |
| 0x0066001E | settings | see table below | ^[raw/docs/taito-f3/scroll-regs.txt] |

Settings register bits (`[???? ???? e??? ??bl]`): ^[raw/docs/taito-f3/scroll-regs.txt]

| Bit | Name | Meaning |
|---|---|---|
| 0 (`l`) | lock | "1 = lock ?": all further writes to this register are ignored until the next reset; also clears blank and extend mode |
| 1 (`b`) | blank | blank video: no colour signals, invalid sync output |
| 7 (`e`) | extend | 0 = 32x32 playfields, 1 = 64x32. "afaik there's no disadvantage to enabling this" |

"afaik, no other bits do anything." ^[raw/docs/taito-f3/scroll-regs.txt]
MAME's file header agrees on the layout: words 0-3 X scroll, 4-7 Y scroll, 8-11 unused (always 0),
word 12 pivot X, word 13 pivot Y, word 14 unused, word 15 "If set to 0x80, then 1024x512 playfields are used, else 512x512";
"writing to low 4 bits is desync or blank tilemap". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
MAME takes extend mode from a per-game table (`f3_config_table`) rather than from the register
write. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Hypothesis for the recompiler: because the registers are write-only on the bus, a runtime only needs
to latch the last value written; the lock bit means a later write must be ignored after bit 0 is set
(the notes say reset clears it). ^[raw/docs/taito-f3/scroll-regs.txt]

## Data formats at a glance

| Structure | Where | Page |
|---|---|---|
| sprite entry, 8 words | 0x600000 | [[hardware/sprites]] |
| playfield tile, 2 words (32 bits) | 0x610000 | [[hardware/tilemaps]] |
| text tile, 1 word; glyph rows, 1 long per row | 0x61C000 / 0x61E000 | [[hardware/pivot-layer]] |
| line-RAM latch and values | 0x620000 | [[hardware/line-ram]] |

Sources: ^[raw/docs/taito-f3/graphics-structs.txt] ^[raw/docs/taito-f3/website/fdp-memory.html]

## Sprite framebuffer

The four DRAM chips to the left of the FDP (IC5-IC8, "fpm ram") are the sprite framebuffer; the
two chips above it (IC11/IC12) are CPU-visible. ^[raw/docs/taito-f3/website/fdp.html]
^[raw/docs/taito-f3/draw.txt] The sprite layer is 12bpp from a single bitmap, which is why sprites of
different groups can neither overlap nor blend with each other. ^[raw/docs/taito-f3/graphics.txt]
Sprites are assumed to be double-buffered: rendered one frame, displayed the next
(`[INFERENCE]` in the notes: "it is assumed"). ^[raw/docs/taito-f3/website/fdp/sprite.html]
MAME models a per-game "sprite lag" of 0-2 frames, ordering sprite parse/draw against the scanline
renderer, and marks it "TODO: presumably ... timing of sprite ram/framebuffer access".
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Pin summary

Full pinout (with the guessed CPU address/data multiplexing through the FCM): see
`raw/docs/taito-f3/fdp.txt` and the table in `raw/docs/taito-f3/website/fdp.html` (12Me21 notes
"some of these pin assignments are only approximately correct"; directions come from die images).
^[raw/docs/taito-f3/fdp.txt] ^[raw/docs/taito-f3/website/fdp.html] Groups:

| Pins | Function |
|---|---|
| 1-9, 11 | CPU interface: chip-select (from PAL D77-01), AS/DSACK-derived strobe, DS, A14..A8 (the notes: no higher/lower address pins from the CPU; the rest go through the FCM) ^[raw/docs/taito-f3/fdp.txt] |
| 12-14, 16-26, 28-29 | multiplexed address/data to the [[hardware/fcm]] ^[raw/docs/taito-f3/fdp.txt] |
| 30-51 | graphics RAM (R/W, CE, A16..A0 shared with data lines; "graphics ram A8/A10 are swapped") ^[raw/docs/taito-f3/website/fdp.html] |
| 53-111 | the four sprite-framebuffer RAMs and their RAS/CAS/OE ^[raw/docs/taito-f3/fdp.txt] |
| 118 | frame pulse: high near the end of scanline 255, low near the end of scanline 4 (11 lines later); it drives the vblank interrupts and is *not* vertical sync ^[raw/docs/taito-f3/website/fdp.html] |
| 120 | video sync ^[raw/docs/taito-f3/website/fdp.html] |
| 123-124 | pixel clock (6.6715 MHz) and half-pixel clock (13.343 MHz) ^[raw/docs/taito-f3/website/fdp.html] |
| 125-179 | graphics ROM / cartridge bus ("tile/sprite" select on 180/207) ^[raw/docs/taito-f3/website/fdp.html] |
| 181-205 | outputs to the [[hardware/fda]] / colour RAM: `color0..12`, `alpha0..3`, `blur`, `mode0/1` ^[raw/docs/taito-f3/website/fdp.html] |
| 206 | CPU reset ^[raw/docs/taito-f3/fdp.txt] |

A small undumped PAL (D77-07) sits between video sync / frame pulse and one FDP input and appears to
make a delayed low pulse starting 16 px before the end of vertical sync and lasting 232 px.
^[raw/docs/taito-f3/website/fdp.html] Video timing: 432 px x 262 lines, pixel clock 6.6715 MHz
(~58.94 Hz); see [[hardware/video-timing]]. ^[raw/docs/taito-f3/website/video.html]

## Terminology

The notes' terms (half-pixel, priority cell, blend mode, alpha select, subglobal scroll, …) are
collected in [[hardware/glossary]]. ^[raw/docs/taito-f3/terms.txt]

## Open points

- Texture ROM addressing: sprite textures read 24 bits per address (4 pixels at 6bpp, 64 addresses
  per texture, 18-bit texture id); tile textures read 48 bits per address (8 pixels, 32 addresses,
  bit 0 of the graphics bus unused, 18-bit id); whether the bus is wider than 24 address bits is
  unknown ("our cartridge only latches that many"). ^[raw/docs/taito-f3/sprite-ram.txt]
- The "pivot port" (0x621000-0x62FFFF region) is only partly understood, see [[hardware/line-ram]].
  ^[raw/docs/taito-f3/line-ram.txt]
- Palette RAM and the FDA are covered in [[hardware/fda]].
