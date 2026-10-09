---
title: Sprite "Enable Scrolls" value 2 adds both scrolls (notes) vs adds none (MAME)
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [sprite, emulator-bug, undocumented]
sources:
  - raw/docs/taito-f3/website/fdp/sprite.html
  - raw/docs/taito-f3/sprite-ram.txt
  - raw/emu-source/mame-0.289/taito_f3_v.cpp
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/sprite.html, note: "Enable Scrolls value 2 listed as 'global and subglobal (yes, really)'"}
  - {kind: doc, ref: raw/docs/taito-f3/sprite-ram.txt, note: "ignore bits 0b1000 behave like 0b0000, i.e. use both offsets"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "sprite_axis::update, bit 3 of the scroll nibble skips adding any scroll (L1303-L1316)"}
contradictions:
  - "Notes: sprite word 2 bits 15..14 = 2 adds global+subglobal. MAME: bit 15 set adds nothing."
supersedes: []
---

# Sprite "Enable Scrolls" value 2

Sprite word 2 holds `i:2,s:2,x:12`. The top two bits (`i`) choose which scroll values are added to
the sprite's position ^[raw/docs/taito-f3/website/fdp/sprite.html].

| `i` (bits 15..14) | Notes | MAME 0.289 |
|---|---|---|
| 0 | global + subglobal | global + subglobal |
| 1 | global only | global only |
| 2 | global + subglobal ("yes, really") | none |
| 3 | none | none |

Sources: notes ^[raw/docs/taito-f3/website/fdp/sprite.html; raw/docs/taito-f3/sprite-ram.txt]; MAME
tests bit 3 of the 4-bit nibble (`!BIT(scroll, 3)`) before adding anything, so any value with bit 15
set skips both scrolls ^[raw/emu-source/mame-0.289/taito_f3_v.cpp L1303-L1316].

Neither side is confirmed. A game that sets `i = 2` on a visible sprite would show the difference
directly. Hypothesis: no shipped game uses `i = 2`, which would explain why MAME's reading has not
been noticed; this has not been checked against any program ROM. ^[raw/docs/taito-f3/sprite-ram.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

See [[comparisons/hardware-vs-mame-video]] and [[hardware/sprites]].
