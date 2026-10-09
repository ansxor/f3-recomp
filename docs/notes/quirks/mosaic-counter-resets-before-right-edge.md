---
title: Mosaic sample counter resets two pixels before the right screen edge
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [video, line-ram, timing]
sources: [raw/docs/taito-f3/website/fdp/mosaic.html, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/mosaic.html, note: "'the effect is reset 2 pixels before the right edge of the screen'"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "mosaic() with 'hw quirk: the counter resets 2 px from the right edge'"}
contradictions: []
supersedes: []
---

# Mosaic sample counter resets two pixels before the right screen edge

Falsifiable claim: the horizontal mosaic sample-and-hold has fixed sampling columns, and its phase counter
restarts 2 pixels before the right edge of the 320-pixel line, so the last two columns start a new sample
group. ^[raw/docs/taito-f3/website/fdp/mosaic.html]

MAME models this as `x_count = screen_x + 114`, subtract 432 when ≥ 432, and sample column
`x − (x_count % interval)`; 432 − 114 = 318, i.e. the wrap happens at screen column 318. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
The arithmetic for column 318 is this page's inference. Which scanline's mosaic parameters apply at the
edge is unchecked. ^[raw/docs/taito-f3/website/fdp/mosaic.html]

See [[hardware/clip-and-mosaic]] and [[hardware/video-timing]].
