---
title: Blank bit in FDP settings register produces invalid sync
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [video, timing, undocumented]
sources:
  - raw/docs/taito-f3/scroll-regs.txt
  - raw/docs/taito-f3/vtiming_dropout_1.txt
  - raw/docs/taito-f3/vtiming_dropin_1.txt
  - raw/docs/taito-f3/website/video.html
games: []
addresses: [0x0066001E]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/vtiming_dropout_1.txt, note: "write $0002 to $66001E: output becomes 7128 px cycles with no vsync"}
  - {kind: doc, ref: raw/docs/taito-f3/vtiming_dropin_1.txt, note: "write $0000 afterwards: bad frames then a normal frame"}
contradictions: []
supersedes: []
---

# Blank bit in FDP settings register produces invalid sync

**Claim (hypothesis):** writing `0x0002` to `0x0066001E` (FDP settings register, bit `b` "blank video") removes colour output and also breaks the sync output: after the last normal frame the monitor sees repeating 7128-pixel cycles (16.5 scanlines) without a vsync pulse. Writing `0x0000` restores a normal frame after a few bad ones.^[raw/docs/taito-f3/scroll-regs.txt] ^[raw/docs/taito-f3/vtiming_dropout_1.txt] ^[raw/docs/taito-f3/vtiming_dropin_1.txt]

- Bit 0 (`l`) locks the register until the next reset and clears blank and extend mode; bit 7 (`e`) is extend (64x32 playfields instead of 32x32). Layout `[???? ???? e??? ??bl]` is 12Me21's, "afaik, no other bits do anything".^[raw/docs/taito-f3/scroll-regs.txt]
- The same invalid signal is what the machine outputs for several frames after power-up.^[raw/docs/taito-f3/website/video.html]
- Falsifiable by: measuring sync on a board while a game writes `0x0002`.
- Hypothesis (software relevance): a game blanking via this bit gets an invalid sync signal on real hardware; the CPU-visible interrupt behaviour during blank is not documented in the notes. Register layout: [[hardware/fdp]]; normal sync timing: [[hardware/video-timing]].
