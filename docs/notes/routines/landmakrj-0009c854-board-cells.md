---
title: Land Maker board cell grid (sub_09c854 board init)
created: 2026-10-15
updated: 2026-10-15
type: routine
tags: [game-loop, object-system]
sources: [raw/tests/landmakrj-board-and-piece-queue-probe.md]
games: [landmakrj]
addresses: [0x0009C854, 0x0009C244, 0x0009C260, 0x0009C474, 0x0009C52E, 0x00093A1C, 0x004073B6, 0x00407676, 0x00407376, 0x00407636, 0x004075F5, 0x004078B5, 0x0040715C, 0x0040715E, 0x00408000]
status: hypothesis
evidence: []
contradictions: []
supersedes: []
---

# Land Maker board cell grid (sub_09c854)

`sub_09c854` initialises the two per-player block boards in work RAM. This page records the layout
and the cell encoding as read from static disassembly and from two 1P-versus-CPU runs with mashed
input. Everything here is a hypothesis from one game mode; see "Unknowns". The piece queue is on
[[routines/landmakrj-00093a1c-piece-generator]]; the game page is [[games/landmakrj]].

## Addresses

`a5` = 0x408000. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

| What | P1 | P2 |
|---|---|---|
| Row 0 base | 0x4073b6 (`a5-$c4a`) | 0x407676 (`a5-$98a`) |
| Launcher rows -2..-1 | 0x407376 (`a5-$c8a`) | 0x407636 (`a5-$9ca`) |
| 640-byte read range (launcher + 18 rows) | 0x407376..0x4075f5 | 0x407636..0x4078b5 |

^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

`cell(R, C) = base + 0x20 * R + 4 * C`: 8 cells per row, 4 bytes per cell, row stride 0x20.
^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

Do not read P2 past row 17: P2 rows 18 and above (0x4078b6..) overlap the character-select state at
0x4078f0..0x40791f. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Init (sub_09c854)

- Clears 0xa0 longs per player from each base (`move.w #$9f,d0; clr.l (a0)+; clr.l (a1)+; dbra`),
  which is 20 rows. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
- Writes the word 0xffff (a wall) at C = 0, 1, 7 (`$0`, `$4`, `$1c`) on rows 0..16, and at C = 6 (`$18`)
  only when `btst.b #0,d0` is true on the down-counter 16..0. Row 17 gets 0xffff in all 8 cells.
  ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
- Finally clears 16 longs at 0x407376 and at 0x407636 (launcher rows -2..-1).
  ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Playable cells

A dump of run A frame 3250 shows walls (`ffff0000`) at C = 0, 1, 7 on every row 0..16, at C = 6 on odd
rows, and a full wall row at row 17. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

- Even R: C = 2..6 (5 cells). Odd R: C = 2..5 (4 cells); a honeycomb. Playable rows 0..16, row 17 is the
  end wall. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
- `sub_09c474` picks neighbour offsets `$1c/$20` versus `$20/$24` by row parity (`btst.b #0,d1`) and
  indexes `$40(a0,d6.w*4)` with `d6 = row*8 + col + 2`. The constant `$40` is two row strides; how the
  object row field `$38(a3)` relates to R is not determined. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
- A block fired from the launcher was seen at row 1, then 5, then 8 (run B frames 2002, 2008, 2016; P1
  cell value `00843e2c`), so R grows away from the player. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
  Row 17 being the top of the screen is [INFERENCE]: one screenshot match (run A frame 3000).
- Shots in flight are written into and cleared from the cells along their path, so a dump can contain an
  in-flight block. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
- A held block was seen in the launcher row: `008a3f14` at 0x40737e (row -2, C = 2) in run B frame 2066.
  ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Cell encoding `FF TT PPPP`

| Byte(s) | Observation |
|---|---|
| byte0 `FF` | 0x00 normally. 0xff on whole P1 rows growing from row 16 downward two rows at a time |
| byte1 `TT` | 0x00 empty, 0xff wall (word 0xffff), 0x8N or 0x9N block, 0xb0..0xb6 special |
| bytes 2-3 `PPPP` | low 16 bits of the block object address; full address = 0x400000 + PPPP |

^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

**byte0 FF.** In run A (P1, dumps every 50 frames) no row had it at frame 2950, rows 15-16 had it at
3000..3200 and rows 13-16 at 3250..3650; P2 had none. Clears preserve it:
`andi.l #$ff000000,(a0,d0.w*4)` at 0x09c24c (`sub_09c244`) and in `sub_09c260`. The wall words
(`ffff0000`) are unchanged by it. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
The meaning is unknown and no writer was found; the pusher-lowering-the-ceiling idea is [INFERENCE].

**TT.** Blocks 0x8N were seen only on P1 and 0x9N only on P2; the player split is [INFERENCE] from one
run. Observed types over gameplay frames 1750..3650: P1 {0x84, 0x86, 0x8a, 0x8e}, P2 {0x92, 0x94, 0x96,
0x9a, 0x9e, 0xb0}. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
`sub_093a1c` excludes 0xb0..0xb6 (`cmpi.w #$b0 / #$b6`) from the colour mask, and `sub_09c474` sends cells
whose word is in 0xb0..0xb6 to `sub_09c52e` (it uses the object's `$6e` against `$28`; not decoded here).
^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

**PPPP.** `sub_09c474`/`sub_09c52e` build the object pointer with `move.l a5,d3; move.w $42(a0,d6.w),d3`.
The object fields `$36/$38/$24/$26` read there are (col/row/player/type) [INFERENCE]; `$26` is compared
with 0xb0..0xb6. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

**Colour.** The low nibble of TT gives the colour code; the mask bit is `(TT & 0xf) >> 1`
(`andi.w #$f; asr.w #1; bset`) and `sub_093a1c` draws from ROM table 0x093af8 = {0, 2, 4, 6, 8, 10, 14}.
Code 4 looked orange and 10 yellow on one screenshot; that and all other colour names are [INFERENCE] or
unknown. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
Blocks change colour in place (same PPPP): P1 (9,4) `008a317c` -> `0084317c` at frame 2052; (7,3) and
(7,4) 0x84 -> 0x8a at frame 2922 (run B). ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Unknowns

- byte0 0xff writer and meaning: a `mametap write 0x407596:0x4075d5` run during a match is not done.
- Colour names: dense dumps with `rendered.png`.
- The 8x/9x player split and the 0xb0 specials: a 2P versus run (`2p-versus-rounds.txt`) is not done.

See also [[routines/landmakrj-0008e20c-round-task]].
