---
title: Interrupt 5 interval timer (0x4C0000) programming
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [interrupt, timing, undocumented, emulator-bug]
sources:
  - raw/docs/taito-f3/interrupt.txt
  - raw/docs/taito-f3/interrupts2.txt
  - raw/emu-source/mame-0.289/taito_f3.cpp
games: []
addresses: [0x004C0000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/interrupt.txt, note: "measured int 5 interval per register value"}
  - {kind: doc, ref: raw/docs/taito-f3/interrupts2.txt, note: "cycles = 0x4000 - rate*8 + 32"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "MAME logs the write and raises no level 5"}
contradictions:
  - "MAME never raises interrupt 5; hardware notes say a write with bit 13 set starts a periodic level 5 interrupt."
supersedes: []
---

# Interrupt 5 interval timer (0x4C0000) programming

**Claim (hypothesis):** a 16-bit write to `0x004C0000` with bit 13 set starts a periodic autovector level 5 interrupt every `0x4000 - rate*8 + 32` CPU cycles (`rate` = low 12 bits; CPU cycle = 0.0656 µs). With bit 13 clear no level 5 interrupt arrives.^[raw/docs/taito-f3/interrupts2.txt] ^[raw/docs/taito-f3/interrupt.txt]

- Measured anchor points: `0x278B` → 64.14 µs; `0x2700` → 137.5 µs; `0x2000` → 1077 µs; `0x0100`/`0x0000` → none.^[raw/docs/taito-f3/interrupt.txt]
- Rates `0xF8B`, `0xF00`, `0xC8B`, `0xC00` (written as `0x2F8B`, `0x2F00`, `0x2C8B`, `0x2C00`) fire every 5.316 µs and cause a reset loop; the formula would give a negative interval, so the hardware behaviour for such rates is outside the formula.^[raw/docs/taito-f3/interrupt.txt]
- A scanline is 986.7 CPU cycles, so rate `0x788.A9` ≈ one line; games write `0x78B` (`0x278B` with enable).^[raw/docs/taito-f3/interrupts2.txt]
- Software relevance: games that write `0x278B` at POST expect a pseudo-hblank level 5 every ~64 µs; MAME ignores the write, so a runtime following MAME will not deliver level 5.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Falsifiable by: a game ROM that installs a level 5 handler and writes `0x278B`; check whether the handler is reached on hardware captures.

See [[hardware/interrupts]], [[hardware/video-timing]].
