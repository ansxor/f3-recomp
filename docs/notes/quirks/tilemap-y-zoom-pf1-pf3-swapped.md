---
title: PF1 and PF3 vertical zoom are stored in each other's line register
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [tilemap, line-ram, undocumented]
sources: [raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/website/fdp/lineram.html, raw/docs/taito-f3/website/fdp/tilemap.html, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/line-ram.txt, note: "8200: 'Y - {playfield 3} y scale - BUG'; 8600: 'Y - {playfield 1} y scale - BUG'"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "FIX_Y[] = {0,3,2,1} maps each zoom word to the playfield whose Y zoom it holds; header: 'playfield 2 Y zoom is stored where you would expect playfield 4 y zoom'"}
contradictions: []
supersedes: []
---

# Playfield 1 / 3 vertical zoom swap

Line registers `8000`-`8600` each hold an X zoom (bits 15:8) and a Y zoom (bits 7:0) for one
playfield, numbering playfields 0-3. Registers `8000` and `8400` hold PF0's and PF2's X and Y
zoom as expected. Register `8200` holds PF1's X zoom but **PF3**'s Y zoom; register `8600` holds PF3's
X zoom but **PF1**'s Y zoom. The notes mark both as "BUG". ^[raw/docs/taito-f3/line-ram.txt]
^[raw/docs/taito-f3/website/fdp/lineram.html] ^[raw/docs/taito-f3/website/fdp/tilemap.html]
MAME applies the same permutation (`FIX_Y = {0, 3, 2, 1}`), stating that "playfield 2 & 4
registers seem to be interleaved" in its 1-based numbering. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Consequence for a runtime: to zoom PF1 vertically a game writes the low byte of `8600`; to zoom PF3
it writes the low byte of `8200`. A renderer that reads Y zoom from the same word as X zoom would
mis-scale those two playfields. (Inferred from the cited tables.) ^[raw/docs/taito-f3/line-ram.txt]

See [[hardware/line-ram-registers]], [[hardware/tilemaps]].
