---
title: FDP sprite command word (sprite settings register)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, sprite, undocumented]
sources: [raw/docs/taito-f3/website/fdp/sprite.html, raw/docs/taito-f3/sprite-ram.txt, raw/docs/taito-f3/website/fdp-memory.html, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: [0x00604000, 0x0060C000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/sprite-ram.txt, note: "bit-by-bit hardware experiments on word 5 of command sprites"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/sprite.html, note: "sprite settings register layout (draft, many '?')"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "get_sprite_info(): command bits 13, 9:8, 1, 0 and jump ordering"}
contradictions:
  - "Bits 9:8: sprite.html and MAME = texture bit planes (00 4bpp, 01 5bpp, 11 6bpp); sprite-ram.txt lists them as 'f'/'g' with garbage-palette effects."
  - "Bit 10: sprite-ram.txt = bank-select-2 (list at 0x604000); sprite.html names it b0, 'lower bit' of the bank number; MAME ignores it."
  - "Bit 1: sprite.html 'supposedly disables clearing framebuffer but seems to be something else'; fdp-memory.html and MAME = preserve/trails (do not clear framebuffer)."
  - "Bit 12: sprite.html (j) = invert pivot text-tile id bit 0 + timing change; sprite-ram.txt = changes video signal; MAME = 'disabled? (from F2 driver)', logged as unknown."
supersedes: []
---

# Sprite command word

Companion to [[hardware/sprites]] (entry format) and [[hardware/fdp]]. When bit 15 of word 3 is set,
the entry is a *command* and its word 5 is written to a persistent settings register instead of
(or in addition to) supplying the tile id's high bits. ^[raw/docs/taito-f3/website/fdp/sprite.html]
Effects remain until another command sprite changes them. ^[raw/docs/taito-f3/sprite-ram.txt]
`sprite-ram.txt` marks which bits of word 5 affect the current sprite (mask `$00F3`) and which are
persistent settings when in command mode (mask `$3773`). ^[raw/docs/taito-f3/sprite-ram.txt]
A command bit set on a *visible* sprite does both jobs. ^[raw/docs/taito-f3/website/fdp/sprite.html]

## Bits (positions derived by counting the listed field widths MSB-first)

| Bit(s) | Name in notes | Meaning / observation |
|---|---|---|
| 13 | k | 1 = flipscreen (sprite.html, sprite-ram.txt; fdp-memory.html "set screen orientation") |
| 12 | j | sprite.html: 1 = lowest bit of all pivot-layer texture ids is treated as inverted (44 <-> 45, 45 <-> 44) and the video timing shifts slightly; seems to need setting near the start of the frame (sprite index <= 16). sprite-ram.txt: "changes video signal (causes us to lose sync, but still outputting a picture)" |
| 10 | b0 / h | sprite RAM bank bit (see below) |
| 9:8 | bpp / f,g | texture bit planes: 00 = 4bpp, 01 = 5bpp, 11 = 6bpp (fdp-memory.html). sprite-ram.txt: f "sets palettes to 0(?) and adds garbage pixels", g "similar to f but different" |
| 7 | ? / e | sprite-ram.txt: "squish vertically and display bottom different?" |
| 6:4 | a (3 bits) | "affects visibility of sprites or something" (sprite.html); in sprite-ram.txt `d`, `c`, `a` = "display right half different?", "mix up rows?", "flip each group of 4 columns (texture relative)?" |
| 1 | t | framebuffer clear disable (see contradictions) |
| 0 | b1 / b | bank bit; sprite-ram.txt: "(command mode only) set bank" |

^[raw/docs/taito-f3/website/fdp/sprite.html] ^[raw/docs/taito-f3/sprite-ram.txt]
^[raw/docs/taito-f3/website/fdp-memory.html]
Tile id high bits: sprite-ram.txt says "the first 2 bits are the upper bits of the tile id
regardless of whether we're in command mode"; sprite.html places the 2-bit `tex2` field at bits 1:0
of word 5, i.e. the same bits as `t` and `b`, and MAME uses only bit 0 as tile bit 16. A command
sprite is still rendered (sprite-ram.txt). ^[raw/docs/taito-f3/sprite-ram.txt]
^[raw/docs/taito-f3/website/fdp/sprite.html] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Bank switching

sprite.html: going from bank 0 (or 2) to 1 (or 3) takes effect on the *next* frame; the other
direction is immediate; and a frame that started in bank 1 or 3 always returns to 0 or 2, so an odd
bank cannot last two frames in a row (see [[quirks/sprite-odd-bank-lasts-one-frame]]).
^[raw/docs/taito-f3/website/fdp/sprite.html]
sprite-ram.txt: bank select 2 (`h`, bit 10) "uses sprite list at `$604000` (and presumably `$60C000` if
the other bank switch is set)". ^[raw/docs/taito-f3/sprite-ram.txt] Other tested behaviour: with the
`$00F0` nibble set to 3, 5, 11 or 13 all sprites disappear, reappearing with garbage pixels for a frame
when changed back; 3/11 differ from 5/13 in that sprites stay visible when bank-flipping; values 1 and
9 also lose sprites while flipping banks; of the low three bits of that nibble, `.001` hides the other
bank's sprites, `.011` and `.101` hide both banks', and `.000`, `.010`, `.100`, `.110`, `.111` are normal.
^[raw/docs/taito-f3/sprite-ram.txt]
The first lines of `sprite-ram.txt` are an earlier experiment on the same word ("command at 0xA":
value 0x04 flashes different colours, 0x10 gives very corrupted graphics and a broken sync signal,
0x20 is flipscreen; "rest nothing?"). ^[raw/docs/taito-f3/sprite-ram.txt]

## MAME's decoding

Flipscreen = bit 13; extra planes = bits 9:8 (`00` 4bpp, `01` 5bpp, `10` "nonsense", `11` 6bpp, used as
the pen mask `(planes << 4) | 0x0f`); sprite trails = bit 1 (do not clear the framebuffer); bank =
bit 0, selecting the list at word offset 0x4000 (byte 0x8000). Logged as unknown: bits 15:14, 12, 7,
and 4:2 (`cntrl & 0b1101'1100'1111'1100`); "bit 12 (0x1000) = disabled? (From F2 driver, doesn't seem
used anywhere)". The jump bit in word 6 is processed *after* the command (recalh uses both in one entry,
including backwards jumps); a jump to its own index ends the list.
^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
