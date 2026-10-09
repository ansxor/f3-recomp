---
title: F3 video terminology (12Me21 glossary)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, blend, sprite, tilemap, palette]
sources: [raw/docs/taito-f3/terms.txt]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/terms.txt, note: "12Me21's glossary of FDP terms"}
contradictions: []
supersedes: []
---

# F3 video terminology

Terms used across the video pages, as `terms.txt` defines them. The chip is the [[hardware/fdp]];
the blend model these terms describe is on [[hardware/priority-and-blend]].

| Term | Meaning |
|---|---|
| playfield / tilemap layer n | the same thing; the 12Me21 notes avoid Taito's names "SCR" (tilemap) and "OBJ" (sprite) |
| sprite group / sprite priority group | sprites split by the upper 2 bits of palette (`0x00,0x40,0x80,0xC0`); separate blend settings |
| colour id / colour index | 13-bit FDP output that indexes colour RAM |
| palette id | upper bits of a colour index; final colour = `texture colour | palette id << 4` (OR, not add; texture colour 0-63) |
| half-pixel (A/B, left/right) | one of the two pixels sent to the FDA per screen pixel |
| priority cell (theory) | 2x16 array (half-pixel x priority) per pixel; the highest non-empty cell of each half-pixel column wins |
| blend mode | which half-pixels a layer contributes to (neither/A/B/both); encoded differently per layer |
| alpha / blend value | `$7` (100%) to `$F` (0%), `$B` = 50%; 4 values (2 per half-pixel) |
| alpha / blend select | which set of values a layer uses; per tile for playfields, per line elsewhere |
| global scroll / local scroll (scroll adjust, "rowscroll/colscroll") | register vs per-scanline offset; sprites have "global" and "subglobal" scroll |
| palette adjust / palette add | per-line value added to the palette id |
| flipscreen | global H+V flip of most rendering, including line RAM read in reverse order |
| sprite block / block controls / tessellation controls | positioning a sprite relative to the previous one |

^[raw/docs/taito-f3/terms.txt]

"Priority" in `terms.txt` is blend priority; "sprite priority" only matters while rendering into the
framebuffer and is mapped to blend priority by line RAM `7600`. ^[raw/docs/taito-f3/terms.txt]

See also [[hardware/sprites]] (block controls, subglobal scroll) and [[hardware/line-ram-registers]].
