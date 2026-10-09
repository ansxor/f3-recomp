---
title: FDP line RAM (per-scanline settings): structure and latching
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, line-ram, memory-map, timing, undocumented]
sources: [raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/website/fdp/lineram.html, raw/docs/taito-f3/website/line-ram.html, raw/docs/taito-f3/website/fdp-memory.html, raw/docs/taito-f3/website/fdp/home.html, raw/docs/taito-f3/terms.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp, raw/emu-source/mame-0.289/taito_f3.h]
games: []
addresses: [0x00620000, 0x00621000, 0x00624000, 0x00625000, 0x00626000, 0x00627000, 0x00628000, 0x00629000, 0x0062A000, 0x0062B000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/line-ram.txt, note: "latch/section/subsection address and data layouts, per-register bit fields"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/lineram.html, note: "30 line registers table (website draft)"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "header comment and read_line_ram(): fixed section mapping, latch/alt-bit handling"}
contradictions:
  - "Alt-bank bit without latch: line-ram.txt and lineram.html say the alternate-bank bit 'does nothing if latch is 0'; MAME checks the alt bit first and reads the +0x800 list even when the plain latch bit is 0."
  - "Section mapping: the notes say each section can be mapped to any of 16 4 KiB blocks by the 4-bit m field of its latch word; MAME ignores m and hard-wires section s to 0x4000 + 0x1000*s of line RAM. They agree for the usual game setup (m = s + 4)."
  - "Per-line sync of the Y accumulator and other 6000/6400 bits: see [[hardware/line-ram-registers]] for the full list of conflicts between the notes' files and MAME."
supersedes: []
---

# FDP line RAM

Almost every graphics setting that varies per scanline is an entry in line RAM (12Me21 compares it
to SNES HDMA): "there are 30 16-bit Line Registers, and each one is read from an array of 256
values (one for each scanline)". The exceptions are tile data, tilemap/pivot global scroll, extend
mode, video-signal settings, sprite data and textures. ^[raw/docs/taito-f3/website/fdp/lineram.html]
What makes it complex is the *latch* machinery that lets a game reuse one value for many scanlines.
^[raw/docs/taito-f3/website/fdp/lineram.html] The chip is the [[hardware/fdp]]; the 30 registers'
bit fields are in [[hardware/line-ram-registers]]. Per-layer behaviour: [[hardware/tilemaps]],
[[hardware/sprites]], [[hardware/pivot-layer]], [[hardware/priority-and-blend]],
[[hardware/clip-and-mosaic]].

## Location

Line RAM occupies the 64 KiB window 0x00620000-0x0062FFFF. ^[raw/docs/taito-f3/website/fdp-memory.html]

| Offset in window | Contents |
|---|---|
| 0x0000-0x0FFF | latch table: one word per scanline per section (8 x 256 words, `word x 256 x 8`) |
| 0x1000-0x1FFF | "pivot port" area, purpose unknown (see below) |
| 0x2000-0xFFFF | 14 blocks of 0x1000 bytes, "lineram block 2..F" (word x 256 x 4 x 2) |

^[raw/docs/taito-f3/website/fdp-memory.html] The first two mappable positions (m = 0, 1) would
collide with the latch table and pivot port ("though, the first two would interfere with other
things"). ^[raw/docs/taito-f3/line-ram.txt] Every game maps the eight sections at the same place:
section `s` at m = `s + 4`, i.e. 0x624000, 0x625000, ... 0x62B000; the names `4000`..`B000` used
throughout the notes and MAME are the offsets of these sections within the 0x620000 window.
^[raw/docs/taito-f3/website/fdp/lineram.html] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Hierarchy

- 8 **sections** (traditionally `4000, 5000, 6000, 7000, 8000, 9000, A000, B000`), each mappable to
  one of 16 locations (0x620000 + 0x1000 * m). ^[raw/docs/taito-f3/line-ram.txt]
- Each section has 4 **subsections** (traditionally `000, 200, 400, 600`), each 256 words = 0x200
  bytes, one word per scanline. ^[raw/docs/taito-f3/line-ram.txt]
- Each subsection can sit at its base address *or* at base + 0x800 ("alternate bank"). Together that
  is 8 x 4 = 32 slots, minus the two missing in section 0 = 30 registers.
  ^[raw/docs/taito-f3/website/fdp/lineram.html]
- Section 0 lacks `4000` and `4200`, which is why per-line Y scroll adjust and alternate tilemap
  exist only for tilemaps 2 and 3. ^[raw/docs/taito-f3/website/fdp/lineram.html]
- The register naming "section.subsection", e.g. 0.2 = `4400`, 2.1 = `6200`, 7.3 = `B600`, is used
  by the notes. ^[raw/docs/taito-f3/website/fdp/lineram.html]

## Latch table (0x00620000 + section * 0x200 + scanline * 2)

Address `[*010 0000 sssl llll lll-]`: `s` = bits 11:9 (section 0-7), `l` = bits 8:1 (scanline 0-255).
^[raw/docs/taito-f3/line-ram.txt] Word at the same index as MAME's `m_line_ram[(section*0x200)/2 + y]`.
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

| Bits | Field | Meaning |
|---|---|---|
| 13:10 | m | mapping: this section's data lives at 0x620000 + 0x1000 * m |
| 7:4 | a (a3..a0) | subsection n alternate bank: 1 = add 0x800 to the subsection address (only matters if the latch bit is 1) |
| 3:0 | l (l3..l0) | subsection n latch: 1 = read the parameter from line RAM on this scanline; 0 = reuse the previous value |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
fdp-memory.html shows the same word with `?` in bits 15:14, 9:8 and describes the mapping field as
"Section Mapping Position?". ^[raw/docs/taito-f3/website/fdp-memory.html] Bit `n` of `l` corresponds to
subsection `n` (0 = `x000`, 1 = `x200`, ...). ^[raw/docs/taito-f3/line-ram.txt]

"The latched value persists across frames: if the latch is 0 on the first scanline, the value
from the last scanline of the previous frame is used." ^[raw/docs/taito-f3/line-ram.txt] So the FDP
keeps state per register between frames; a recompiler must not reset it at frame start.
(Hypothesis: power-on value is whatever the hardware flip-flops hold; the notes do not say.)

MAME's variant: bits 3:0 select the base list, bits 7:4 the +0x800 list, "in rare cases (e.g.
bubblem), the second set of subsections is latched on using bits 4,5,6,7 for ..800,a00,c00,e00"; and
it comments "this may actually be computed from the upper byte? i.e. `base = 0x400 * BIT(latches, 8, 8)
+ 0x200 * subsection`". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
In MAME the alt bit alone is enough to read the +0x800 list, unlike the notes (see contradictions).

## Value address

`[*010 mmmm auul llll lll-]`: m = bits 15:12 (from the section's latch), a = bit 11 (subsection bank),
u = bits 10:9 (subsection 0-3), l = bits 8:1 (scanline). Normally `m = section + 4` and `a = 0`.
^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
So, with the usual map, register `x_yy` for scanline `y` lives at
`0x620000 + 0x4000 + section*0x1000 + sub*0x200 (+0x800) + y*2`. (Derivation from the address
pattern; MAME's `base = 0x4000 + 0x1000*section + 0x200*subsection` is the same.)
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Each subsection holds 256 words, so the register that applies to scanline `y` is word `y` of its
list. Flipscreen reads line RAM in reverse order (scanline 255 first); MAME iterates
`y = 255 - screen_y`. ^[raw/docs/taito-f3/terms.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Hypothesis: scanlines 256-261 have no entries and the first visible scanline is 23/24 of the 262
(see [[hardware/video-timing]]); MAME says line data typically starts "including clipped lines at the
top of the screen" and most games show 232 lines. ^[raw/docs/taito-f3/website/video.html]
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Pivot port (0x00621000-0x0062FFFF region)

Latch at `[*010 0001 000l llll lll-]` (0x621000 + scanline*2), data `[.... .... llll ....]` = four
latch bits for "some subsection", unknown which ("there's more than 4 subsections"); subsections at
`[*010 0001 uuul llll lll-]` with `u` = 1..7, data unknown. ^[raw/docs/taito-f3/line-ram.txt]
fdp-memory.html labels the 0x621000 window `"pivot port"?`. ^[raw/docs/taito-f3/website/fdp-memory.html]
MAME: "Pivot port (0x1000-2fff) has only one known used address, 0x1000: unknown control word? (usually
0x00f0; gseeker, spcinvdj, twinqix, puchicar set 0x0000)". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Related: [[hardware/pivot-layer]].

## Register map overview

Enable and clip bits are shared in layout between layers; details in
[[hardware/line-ram-registers]]. "x.y" = section.subsection. ^[raw/docs/taito-f3/website/fdp/lineram.html]

| Offset | x.y | Content |
|---|---|---|
| 4400 / 4600 | 0.2 / 0.3 | PF3 / PF4 Y scroll adjust, alternate tilemap select, high bits of clip planes 0-1 / 2-3 |
| 5000-5600 | 1.0-1.3 | clip planes 0-3 left/right low 8 bits |
| 6000 | 2.0 | sprite-group blend modes; pivot/highcolor/background alpha select; Y scroll sync; pivot mode and data bank |
| 6200 | 2.1 | four blend (alpha) values |
| 6400 | 2.2 | mosaic enables and level; shadow; FDA mode/blur |
| 6600 | 2.3 | background colour index |
| 7000 / 7200 | 3.0 / 3.1 | highcolor ("mystery") layer / pivot layer: blend, clip, enable, priority |
| 7400 | 3.2 | sprite layer: group alpha selects, clip settings |
| 7600 | 3.3 | sprite group priorities |
| 8000-8600 | 4.0-4.3 | playfield X/Y zoom (PF1/PF3 Y swapped) |
| 9000-9600 | 5.0-5.3 | playfield palette add |
| A000-A600 | 6.0-6.3 | playfield X scroll adjust (rowscroll) |
| B000-B600 | 7.0-7.3 | playfield blend, clip, enable, priority |

^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
MAME's summary of which latch word belongs to which group, e.g. `0x0000 ... 4000 (column scroll, alt
tilemap, clip)`, `0x0200 ... 5000 (clip planes)`, `0x0400 ... 6000 (blending)`, `0x0600 ... 7000
(pivot and sprite layer mixing)`, `0x0800 ... 8000 (zoom)`, `0x0a00 ... 9000 (palette add)`,
`0x0c00 ... a000 (row scroll)`, `0x0e00 ... b000 (playfield mixing)`, matches the 0x200-stride
latch table above. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Reading it in a recompiler

- The state per register is one latched value per scanline ("a latch per scanline per section"): for a scanline `y` and register R, if the
  latch bit for R's subsection is 1, the effective value is the word at R's address for `y`;
  otherwise it is the effective value of the previous scanline (and of the last scanline of the
  previous frame for `y = 0`). ^[raw/docs/taito-f3/line-ram.txt] ^[raw/docs/taito-f3/website/fdp/lineram.html]
  (Per the notes; MAME differs, next bullet.)
- MAME walks scanlines 0-255 in order and updates a per-frame `f3_line_inf` only where a latch bit
  is set; that struct is constructed at the top of every `scanline_draw`, so cross-frame persistence
  is *not* reproduced (see [[quirks/lineram-latch-persists-across-frames]]).
  ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Many games write the same data for every line instead of using latches ("most games do that
  anyway"). ^[raw/docs/taito-f3/website/fdp/lineram.html]
