---
title: Main CPU interrupts (levels 2, 3, 5)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [interrupt, vblank, timing, cpu, undocumented]
sources:
  - raw/docs/taito-f3/interrupts2.txt
  - raw/docs/taito-f3/interrupt.txt
  - raw/docs/taito-f3/itiming.txt
  - raw/docs/taito-f3/pal7.txt
  - raw/docs/taito-f3/website/fdp.html
  - raw/emu-source/mame-0.289/taito_f3.cpp
  - raw/emu-source/mame-0.289/taito_f3.h
games: [landmakrj]
addresses: [0x004C0000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/interrupts2.txt, note: "levels 2/3/5, timer formula, per-line cycle count"}
  - {kind: doc, ref: raw/docs/taito-f3/interrupt.txt, note: "register value vs measured int5 interval table"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "interrupt2 / trigger_int3 / f3_timer_control_w"}
contradictions:
  - "Interrupt 3 delay: 12Me21 measured ~712 us (~10850 CPU cycles at 15.238 MHz) after interrupt 2; MAME schedules it 10000 CPU cycles later, and on a 16 MHz CPU clock (625 us)."
  - "Interrupt 5: 12Me21 documents a programmable timer at 0x4C0000 (enable bit + rate); MAME does not generate interrupt 5 at all, f3_timer_control_w only logs the write."
  - "interrupts2.txt describes the register as [..e. rrrr rrrr rrrr] but its own comment 'hblank interrupt rate = 0x788.A9 (games use 0x78B)' and interrupt.txt's table show values like 0x278B: enable is bit 13 (0x2000), consistent between the two files; the wiki treats the bit pattern as bit 13 = enable."
supersedes: []
---

# Main CPU interrupts

The 68EC020 receives autovectored interrupts at levels 2, 3 and 5 as far as the notes cover. Display timing is in [[hardware/video-timing]]; the chip that generates the frame pulse is [[hardware/fdp]].

## Sources

| Level | When | Source |
|---|---|---|
| 2 | Start of vblank: near the end of scanline 255, just after the last visible pixel | frame pulse (FDP pin 118) rising edge, hypothesis |
| 3 | ~712 µs (~10850 CPU cycles) after level 2, in the middle of vblank (near the end of "scanline" 4) | frame pulse falling edge, hypothesis |
| 5 | Configurable interval timer controlled by the 16-bit register at `0x4C0000` | unknown chip (hypothesis: FCM or FDP, [[hardware/fcm]]) |

^[raw/docs/taito-f3/interrupts2.txt] ^[raw/docs/taito-f3/pal7.txt]

- The frame pulse "is what controls the vblank interrupts sent the the CPU" (sic); it is high from the end of scanline 255 to the end of scanline 4 (11 scanlines). The assignment of level 2 to the rising edge and level 3 to the falling edge is the wiki's reading of "which is when the cpu interrupts happen" together with the 712 µs figure. See [[quirks/frame-pulse-interrupt-window]].^[raw/docs/taito-f3/website/fdp.html] ^[raw/docs/taito-f3/pal7.txt] ^[raw/docs/taito-f3/interrupts2.txt]
- In the notes, "av5" in `interrupt.txt` means the autovector level 5 interrupt.^[raw/docs/taito-f3/interrupt.txt]

## Interrupt 5 timer (register `0x4C0000`)

- Written as a 16-bit value: `[..e. rrrr rrrr rrrr]`, `e` = 1 enables, `r` = rate.^[raw/docs/taito-f3/interrupts2.txt]
- Interval: `cycles = 0x4000 - rate*8 + 32` CPU cycles (one CPU cycle = 1/(30.47618 MHz/2) = 0.0656 µs). 12Me21 says this matches the measurements "to within ~10 cycles".^[raw/docs/taito-f3/interrupts2.txt]
- A scanline is 986.713 CPU cycles (64.753 µs). Solving the formula for one scanline gives rate `0x788.A9`; games use `0x78B`, i.e. the interrupt fires slightly faster than once per line. 12Me21 calls this the "hblank interrupt".^[raw/docs/taito-f3/interrupts2.txt]
- Measured intervals for various register values (bit 13 clear: no interrupt):^[raw/docs/taito-f3/interrupt.txt]

| Value written | Result |
|---|---|
| `0x018B`, `0x0100`, `0x508B`, `0x5000` | no int 5 |
| `0x2F8B`, `0x2F00`, `0x2C8B`, `0x2C00` | int 5 every 5.316 µs - causes a reset loop |
| `0x278B` | every 64.14 µs |
| `0x2780` | every 69.88 µs |
| `0x2700` | every 137.5 µs |
| `0x248B` / `0x2480` / `0x2400` | 466.5 / 472.2 / 539.6 µs |
| `0x708B`, `0x308B`, `0x208B` | 1004 µs |
| `0x7000`, `0x3000`, `0x2000` | 1077 µs |

- Hypothesis (wiki's reading of the table, checked against the formula): `0x278B` vs `0x2780` differs by 0x0B×8 = 88 cycles = 5.8 µs (table: 5.74 µs); `0x2780` vs `0x2700` by 0x80×8 = 1024 cycles = 67 µs (table: 67.6 µs); `0x248B` gives 16416-0x48B×8 = 7112 cycles = 466.7 µs (table: 466.5 µs). Bits 12 and 14 do not appear to matter (`0x7000`, `0x3000`, `0x2000` are equal) while bit 13 enables.
- The notes also compute `0x4000 - 0x78B*8 + 32 = 968` cycles ≈ 63.5 µs, against the measured 64.14 µs; hypothesis: the ~10-cycle error is the measurement uncertainty quoted above.^[raw/docs/taito-f3/interrupt.txt] ^[raw/docs/taito-f3/interrupts2.txt]
- A scratch line in `interrupt.txt` (`perhaps: cycles = 16380 - x*8 + 32`) is an earlier fit that differs from the later formula in `interrupts2.txt` only in the constant (16380 vs 0x4000 = 16384); the file also has an unexplained `64.14 / 0x10000/10123 = 9.907…` line.^[raw/docs/taito-f3/interrupt.txt] ^[raw/docs/taito-f3/interrupts2.txt]
- Values programmed by games at POST (from a comment in MAME): `0x278B` (darius gaiden, elevator action, puzzle bobble 2/3/4, gekirindan, land maker), `0x0100` (cup final, scfinals, intcup94, kaiserkn, powergoal, spcinv95, cleopatr, kirameki), `0x0000` (ringrage, arabianm, gunlock, quizhuhu, tcobra2, arkretn, puchicar, popnpop), `0x0090` (qtheater, recalh); many games never write it. By the table above only `0x278B` has bit 13 set; hypothesis: the others leave interrupt 5 disabled, and the pseudo-hblank timer is used by the `0x278B` games.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- `landmakrj` is in the `0x278B` list under its MAME name `landmakr`; the wiki has not yet checked what the ROM writes.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- See [[quirks/interrupt5-timer-interval]].

## MAME's model

- Level 2: `set_vblank_int("screen", interrupt2)` calls `set_input_line(2, HOLD_LINE)` at the screen's vblank start, and arms a timer for `10000` CPU cycles.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Level 3: the timer callback `trigger_int3` raises level 3 with `HOLD_LINE`; the comment says "some signal from video hardware? vblank handler will wait until approximately end of vblank for it".^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Level 5: not generated. `0x4c0000..0x4c0003` is write-only; offset 0 only logs, offset 2 pops up a "contact MAMEdev" message. The comment: "Several games configure timer-based pseudo-hblank int5 here at POST".^[raw/emu-source/mame-0.289/taito_f3.cpp]
- MAME's main CPU clock is `16_MHz_XTAL` with a comment that it should be `30.47618_MHz_XTAL / 2`; the file header also lists "Find how this HW drives the CRTC and verify timing of interrupts" as an open issue.^[raw/emu-source/mame-0.289/taito_f3.h] ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Consequence for a runtime: MAME's int3 delay (10000 cycles) is ~8% shorter in cycles and, with the 16 MHz clock, ~12% shorter in time than the measured ~712 µs. Hypothesis: games that wait for level 3 (the MAME comment suggests the vblank handler does) are insensitive to the exact delay, but this is unverified here.^[raw/emu-source/mame-0.289/taito_f3.cpp] ^[raw/docs/taito-f3/interrupts2.txt]

## Related

- Sound CPU interrupts (DUART → level 6) are separate: [[hardware/sound]].
- Reset/halt generation: [[hardware/fio]].
