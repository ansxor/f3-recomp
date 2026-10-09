---
title: Two layers in the same priority cell show the background color
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [video, blend, emulator-bug]
sources: [raw/docs/taito-f3/website/fdp/priority.html, raw/docs/taito-f3/terms.txt, raw/docs/taito-f3/line-ram.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/priority.html, note: "author's priority-cell model: a cell filled twice is invalid and yields the background color"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "MAME writes palette 0 for the destination on equal priority"}
contradictions:
  - "12Me21: conflicted cell contributes the background color (maybe with its alpha select, '?'); MAME: destination pixel palette index set to 0 and its priority recorded."
supersedes: []
---

# Two layers in the same priority cell show the background color

Falsifiable claim: if two layers place a non-transparent pixel in the same (priority row, half-pixel
column) cell at one screen pixel, that cell is marked invalid and, if it is the one chosen, the FDP outputs
the background color instead of either layer. Layer processing order does not matter. ^[raw/docs/taito-f3/website/fdp/priority.html]
The note leaves open whether the alpha select of the background is used here. ^[raw/docs/taito-f3/website/fdp/priority.html]

Related observations: conflicts also count as empty pixels for [[hardware/shadow-mode]], and a
conflicting layer can cut a hole in a shadow effect. ^[raw/docs/taito-f3/line-ram.txt]

MAME comment: "prio conflict = color line conflict? (dariusg, bubblem)" and "a HW 'feature' (bug?) when layers
have a prio conflict … the second layer can reset part of the state (dariusg stage V' clouds)"; the code sets
the destination palette to 0 on equal priority, which is the background only if color 0 is the background
used. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

See [[hardware/priority-and-blend]] for the model.
