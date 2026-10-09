---
title: Sprite "set scroll" applies to the same sprite (visible sprite renders at 2x position)
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [sprite, undocumented]
sources: [raw/docs/taito-f3/sprite-ram.txt, raw/docs/taito-f3/website/fdp/sprite.html, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/sprite-ram.txt, note: "'scroll is applied BEFORE rendering the current sprite(!) (i.e. using the set scroll bits on a visible sprite will render it at 2x the listed position)'"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "sprite_axis::update sets subglobal/global from the raw position, then adds the scroll to the same position"}
contradictions: []
supersedes: []
---

# Sprite set-scroll applies to the same sprite

A sprite-list entry can write the "global"/"subglobal" scroll from its own position (word 2 bits
13:12) *and* add scrolls to its position (bits 15:14). The order is: read the position, write the
scroll from the **un-adjusted** position, then add the scroll(s) to the position. So an entry that
both sets a scroll and has scroll enabled is drawn at `position + new scroll` = twice the listed
position (for a single scroll). ^[raw/docs/taito-f3/sprite-ram.txt]
MAME's `sprite_axis::update` does the same in that order. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Falsifiable test: an entry with set-scroll = global and enable-scrolls = "global" at X = 0x20 should
appear at X = 0x40 (before block controls). Games usually avoid it by pairing a set-scroll with
"use neither" (value 3). ^[raw/docs/taito-f3/website/fdp/sprite.html]
(That last sentence is a hypothesis from the described bit meanings; the notes do not state what
games do.)

See [[hardware/sprites]], [[hardware/fdp]].
