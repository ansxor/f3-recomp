---
title: Frame pulse window and the level 2 / level 3 vblank interrupts
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [interrupt, vblank, timing, undocumented]
sources:
  - raw/docs/taito-f3/interrupts2.txt
  - raw/docs/taito-f3/pal7.txt
  - raw/docs/taito-f3/website/fdp.html
  - raw/emu-source/mame-0.289/taito_f3.cpp
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/interrupts2.txt, note: "int 2 at start of vblank; int 3 ~712 us (~10850 cycles) later"}
  - {kind: doc, ref: raw/docs/taito-f3/pal7.txt, note: "frame pulse high near end of line 255, low near end of line 4"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "int 3 fires 10000 CPU cycles after int 2"}
contradictions:
  - "MAME: int 3 = 10000 cycles after int 2 (16 MHz CPU); 12Me21: ~10850 cycles (~712 us) on hardware."
supersedes: []
---

# Frame pulse window and the level 2 / level 3 vblank interrupts

**Claim (hypothesis):** the vblank interrupt pair is tied to the FDP's frame pulse, an 11-scanline window that starts just after the last visible pixel of scanline 255 and ends near the end of scanline 4. Level 2 arrives at the start of that window and level 3 ~712 µs (11 × 64.75 µs ≈ 10850 CPU cycles at 15.238 MHz) later, in the middle of the blanking interval.^[raw/docs/taito-f3/interrupts2.txt] ^[raw/docs/taito-f3/pal7.txt]

- Falsifiable by: timing the delay between the level 2 and level 3 handlers on hardware (or a cycle-exact core): expected ≈ 10850 cycles, not 10000.
- The frame pulse is the FDP output pin 118; it is not the vertical sync signal, which spans scanlines 3-6 of the video signal.^[raw/docs/taito-f3/website/fdp.html] ^[raw/docs/taito-f3/website/video.html]
- MAME generates level 2 at its vblank start and level 3 with a fixed 10000-cycle timer; it has no frame pulse.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Software relevance: a vblank handler that waits for level 3 "until approximately end of vblank" (comment in MAME) sees the wait end 8% sooner in MAME than on the measured hardware.^[raw/emu-source/mame-0.289/taito_f3.cpp]

See [[hardware/interrupts]], [[hardware/video-timing]], [[hardware/fdp]].
