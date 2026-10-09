---
title: Odd sprite-RAM banks cannot be active two frames in a row
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [sprite, undocumented]
sources: [raw/docs/taito-f3/website/fdp/sprite.html, raw/docs/taito-f3/sprite-ram.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: [0x00600000, 0x00604000, 0x00608000, 0x0060C000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/sprite.html, note: "'when switching from bank 0 (or 2) to 1 (or 3), the effect is delayed until the next frame ... we can't be in an odd bank for 2 frames in a row'"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "MAME toggles a single bank bit immediately (bit 0 of the command word) and ignores the other bit"}
contradictions:
  - "MAME applies the sprite-bank command immediately and has no delayed/odd-bank behaviour; it only models two lists (bit 0 -> word offset 0x4000)."
supersedes: []
---

# Odd sprite banks last at most one frame

The sprite RAM has four banks of 1024 entries, with the bank number set by two bits of a command
sprite's word 5. The notes observe asymmetric switching: going from bank 0 (or 2) to 1 (or 3) takes
effect on the next frame, the other direction is immediate, and if the frame started in bank 1 or 3
the FDP always returns to 0 or 2, so an odd bank cannot be in use for two consecutive frames.
^[raw/docs/taito-f3/website/fdp/sprite.html]
`sprite-ram.txt` independently reports that command bit `h` (bank select 2) uses the list at
`$604000`, "presumably `$60C000`" with the other bank switch. ^[raw/docs/taito-f3/sprite-ram.txt]
"Most games use banks 0 and 2 as a double buffer", so the quirk is mainly relevant to games that use
the odd banks (unknown which). ^[raw/docs/taito-f3/website/fdp/sprite.html]
Falsifiable test: command the FDP into bank 1 (or 3) on one frame and keep it commanded on the next;
the second frame should still render from bank 0/2.

See [[hardware/sprites]] and [[hardware/fdp]].
