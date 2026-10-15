---
title: Land Maker piece queue and generator (sub_093a1c)
created: 2026-10-15
updated: 2026-10-15
type: routine
tags: [game-loop, rng, object-system]
sources: [raw/tests/landmakrj-board-and-piece-queue-probe.md]
games: [landmakrj]
addresses: [0x00093A1C, 0x000938F2, 0x00093462, 0x00093AF8, 0x000939B0, 0x000939F6, 0x00001468, 0x00400826, 0x00401F54, 0x00401F5C, 0x00401FA4, 0x0040204C, 0x00402034, 0x00402036, 0x00401FF4, 0x0040203C, 0x004020DC, 0x004020DE, 0x0040209C, 0x004020E4, 0x004020FC, 0x0040715C, 0x0040715E]
status: hypothesis
evidence: []
contradictions: []
supersedes: []
---

# Land Maker piece queue and generator (sub_093a1c)

Each player has a two-slot queue (in hand, next) refilled by `sub_0938f2` from the colour generator
`sub_093a1c`. There is no bag: no future pieces are stored. The board that the generator reads is on
[[routines/landmakrj-0009c854-board-cells]]; RNG reseeding for netplay is on
[[routines/landmakrj-0008edde-versus-mode-transition]]. All of it is from one 1P-versus-CPU probe and
static disassembly. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Player struct fields

| Field | P1 | P2 | Meaning |
|---|---|---|---|
| struct base | 0x401fa4 | 0x40204c | |
| +0x90.w | 0x402034 | 0x4020dc | colour in hand |
| +0x92.w | 0x402036 | 0x4020de | NEXT colour |
| +0x50.l | 0x401ff4 | 0x40209c | shot/held object pointer |
| +0x98.l | 0x40203c | 0x4020e4 | per-player RNG seed |
| colour mask | 0x40715c (`a5-$ea4`) | 0x40715e (`a5-$ea2`) | bit = code >> 1 |

^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

`+0x50`: 0 = none; 0xffffffff is set at queue shift by `sub_0938f2` (`move.l #$ffffffff,$50(a2)`);
otherwise a RAM object pointer (0x403be8 and 0x4038bc were seen). ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
Colour mask values seen in run B: 0x88 (frame 1750, queue 6,6), 0x24 (codes 4 and 10, queue holding
4 and 10) and 0x04. It is only rewritten when the generator rescans, so it need not equal the current
board's colours [INFERENCE]. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Queue shift (sub_0938f2)

`bsr.w $93a1c` draws `d0`; then `move.w $92(a2),d4; move.w d0,$92(a2); move.w d4,$90(a2)`: +0x90 takes the
old +0x92 and +0x92 takes the new draw. `sub_093462` (round init) seeds +0x92 for P1 then P2 via
`sub_093a1c` and calls `sub_0938f2` for P1. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
Run B check: the fired block's colour equalled the +0x90 value at the preceding queue change in 5 of 5
shots (fires at about 2002, 2206, 2386, 2520, 2872; changes at 1870 (4,a), 1998 (a,a), 2066 (a,4),
2202 (4,4), 2216 (4,a), 2382 (a,a)). ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Generator (sub_093a1c)

1. If the stage word 0x401f54 is 0xffff or 0xfffe, the colour comes from a fixed ROM sequence at 0x0939b0 /
   0x0939f6 indexed by 0x4020fc (`addq #2` per draw). The tables were not decoded.
   ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
2. Otherwise, if the round-winner word 0x401f5c is nonzero it returns colour 0 (path at 0x093ae6).
   ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
3. It saves the global LCG 0x400826 in d7 and loads the player's +0x98 into 0x400826.
   ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
4. If +0x94 == 0: set +0x94 = 1, clear the mask, scan 20 rows x 8 cells from the board base; every cell
   with a nonzero word at +2 and a type outside 0xb0..0xb6 sets bit `(type & 0xf) >> 1`. Otherwise
   clear +0x94 and use the stored mask. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
5. An empty mask gives colour 0. Otherwise loop: `jsr sub_001468`, `divu #7`, the remainder indexes the
   table at 0x093af8 (words {0, 2, 4, 6, 8, 10, 14}), retry until `btst` finds the bit in the mask.
   ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]
6. Finally +0x98 <- 0x400826 and 0x400826 <- d7 (the global LCG is restored).
   ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

So the colour is a rejection-sampled pick (uniform [INFERENCE]) over the colour codes present on the player's own board, driven by a
per-player seed. Only +0x90 and +0x92 are observable; nothing beyond NEXT is stored.
^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Observation list for an agent

Per frame read: P1 0x407376..0x4075f5 (640 B), P2 0x407636..0x4078b5 (640 B), the +0x90/+0x92 words per
player; optionally the masks 0x40715c/0x40715e and the +0x50 pointers. The summary table is on
[[games/landmakrj]]. ^[raw/tests/landmakrj-board-and-piece-queue-probe.md]

## Unknowns

- The fixed sequences (stage 0xffff/0xfffe) and the field meaning of +0x94 beyond the scan toggle.
- Whether P2 and the 2P versus mode use the same generator path: not tested.
