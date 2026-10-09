---
title: Shadow mode 3 sets color bit 12 on the pivot layer but clears it on sprites
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [video, palette, undocumented]
sources: [raw/docs/taito-f3/website/fdp/shadow.html, raw/docs/taito-f3/shadow.txt]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/shadow.html, note: "table and bit diagram, marked '(!)' by the author"}
  - {kind: doc, ref: raw/docs/taito-f3/shadow.txt, note: "'onto pivot: mode3 = SETS bit12'; 'onto sprite: mode3 = CLEARS bit12'"}
contradictions: []
supersedes: []
---

# Shadow mode 3 sets color bit 12 on the pivot layer but clears it on sprites

Falsifiable claim (hardware-tested per the notes; no game uses shadow mode 3): with line register 2.2
shadow field = 3 and a pivot-layer pixel as the upper layer of a neighbouring-priority pair, the resulting
color id is `1 0000 0000 tttt` (bit 12 set, palette bits cleared); for sprites bit 12 is normally hard-coded
to 1 and shadow mode 3 clears it. ^[raw/docs/taito-f3/website/fdp/shadow.html] ^[raw/docs/taito-f3/shadow.txt]

Context: full bit tables in [[hardware/shadow-mode]]; the pivot layer is [[hardware/pivot-layer]].
