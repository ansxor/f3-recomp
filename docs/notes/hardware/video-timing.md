---
title: Video timing, sync and frame pulse
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, timing, vblank, pcb]
sources:
  - raw/docs/taito-f3/website/video.html
  - raw/docs/taito-f3/vtiming2.txt
  - raw/docs/taito-f3/vtiming3.txt
  - raw/docs/taito-f3/vtiming3.js
  - raw/docs/taito-f3/vtiming4.txt
  - raw/docs/taito-f3/vtiming_startup.txt
  - raw/docs/taito-f3/vtiming_startup_1.txt
  - raw/docs/taito-f3/vtiming_startup_2.txt
  - raw/docs/taito-f3/vtiming_dropin_1.txt
  - raw/docs/taito-f3/vtiming_dropout_1.txt
  - raw/docs/taito-f3/pal7.txt
  - raw/docs/taito-f3/interrupts2.txt
  - raw/docs/taito-f3/scroll-regs.txt
  - raw/emu-source/mame-0.289/taito_f3.cpp
  - raw/docs/taito-f3/website/fdp.html
games: []
addresses: [0x0066001E]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/video.html, note: "oscilloscope-derived sync table, pixel clock, line/frame counts"}
  - {kind: doc, ref: raw/docs/taito-f3/vtiming_startup.txt, note: "startup/garbled frame structure and 12Me21's model of the sync generator"}
  - {kind: doc, ref: raw/docs/taito-f3/pal7.txt, note: "frame pulse (FDP pin 118) window"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "set_raw() screen parameters and visible areas"}
contradictions:
  - "vtiming2.txt counts 252 'normal' (long-gap) scanlines per frame; video.html's table has 262 total scanlines, of which 11-22 and 24-261 have the long gap. The notes do not reconcile the two counts."
  - "MAME's visible area starts at x=46,y=24 while video.html puts the image start 64 px after hsync end (different x origin); y=24..255 matches video.html lines 24-255."
supersedes: []
---

# Video timing, sync and frame pulse

Scope: what the video output and the TC0630FDP "frame pulse" look like in time. The software-visible consequences (when the vblank interrupts fire, how long the CPU has) are in [[hardware/interrupts]]; the chip itself is [[hardware/fdp]]; clocks are in [[hardware/clocks]].

## Clocks and frame size

- Pixel clock 6.6715 MHz, 432 pixels per scanline (~15.44 kHz), 262 scanlines per frame (~58.94 Hz). Sync is active low.^[raw/docs/taito-f3/website/video.html]
- The pixel clock is the 26.686 MHz crystal divided by 4; one pixel is 0.14989 µs, which is the constant used by `vtiming3.js` to convert measured seconds to pixels.^[raw/docs/taito-f3/interrupts2.txt] ^[raw/docs/taito-f3/vtiming3.js]
- Measured scanline period is 64.753 µs, i.e. 432 pixels; hsync pulse low time ≈4.79 µs (32 px), high time ≈59.96 µs (400 px) for a normal line.^[raw/docs/taito-f3/vtiming3.txt] ^[raw/docs/taito-f3/interrupts2.txt]
- Relative to the main CPU (notes assume 30.47618 MHz / 2 = 15.238 MHz) a scanline is 986.713 CPU cycles.^[raw/docs/taito-f3/interrupts2.txt]
- MAME: `set_raw(26.686 MHz / 4, htotal 432, hbstart 46, hbend 320+46, vtotal 262, vbstart 24, vbend 232+24)` and a refresh of 58.94 Hz; the comment cites measurements from an arcade-projects thread on the F3 sync.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- MAME's `f3_224a/b/c` variants only change the visible window (`set_visarea(46, 40*8-1+46, 31|32|24, +224-1)`) for games that program a 224-line display; the comment says no game changes the registers during play.^[raw/emu-source/mame-0.289/taito_f3.cpp]

## Scanline structure (12Me21's table)

Scanline numbers are the notes' own; the frame pulse and the vblank interrupts are anchored to them.

| Scanline | Signal |
|---|---|
| 0 | long hsync (32 px), 184 px, long hsync, 184 px |
| 1, 2 | short hsync (16 px low + 16 px high), 184 px, short hsync, 184 px |
| 3 | as 1-2 but with vsync start in the second half |
| 4, 5 | as 1-2 |
| 6 | as 1-2 but with vsync end in the second half |
| 7-9 | as 1-2 |
| 10 | short hsync, 400 px |
| 11-22 | long hsync, 400 px (blank) |
| 23 | long hsync, 64 px blank, 319 px blank, then 1 px of image, 16 px blank |
| 24-254 | long hsync, 64 px blank, **320 px image**, 16 px blank |
| 255 | long hsync, 64 px blank, 319 px image, 1 px missing, 16 px blank |
| 256-261 | long hsync, 400 px |

^[raw/docs/taito-f3/website/video.html]

- Per line in the visible area: end of hsync, ~63.5 blank pixels, 320 image pixels, ~16.5 blank pixels, start of next hsync.^[raw/docs/taito-f3/vtiming_startup.txt]
- Scanline 23 outputs one pixel of image (most games hide it); scanline 255 is missing one pixel (some games hide it).^[raw/docs/taito-f3/website/video.html]
- Scanlines 0-9 have two hsync pulses each, scanlines 1-10 have the shorter (16 px) pulses; vsync is XORed onto the sync line rather than lengthening pulses, which is why the equalizing pulses look wrong compared with NTSC field 2.^[raw/docs/taito-f3/website/video.html]
- Raw pulse trains (`low high` in pixels): a normal frame is 252 lines of `400 32` (counted in `vtiming4.txt`), preceded by the 16/200 half-line pulses of the vblank; `vtiming2.txt` records the same 32/184 special half scanline, five 16/200 pulses on each side of vsync and a 216 px "inverted" space for the vsync window.^[raw/docs/taito-f3/vtiming4.txt] ^[raw/docs/taito-f3/vtiming2.txt]

## Frame pulse (FDP pin 118)

- The FDP output `118` ("frame pulse") goes **high near the end of scanline 255** (just after the last visible pixel) and **low near the end of scanline 4**, 11 scanlines later. It "controls the vblank interrupts sent to the CPU" and is not vertical sync.^[raw/docs/taito-f3/website/fdp.html] ^[raw/docs/taito-f3/pal7.txt]
- 11 scanlines × 64.75 µs ≈ 712 µs, which matches the delay 12Me21 measured between interrupt 2 and interrupt 3 (see [[quirks/frame-pulse-interrupt-window]]).^[raw/docs/taito-f3/interrupts2.txt]
- PAL D77-07 (IC33, undumped, PALCE16V8Q-15): pins 1+2 are the FDP sync output `120` (the video sync as sent to the monitor), pin 3 is the frame pulse `118`, pin 19 returns to FDP `121`; four pins output 8/4/2/1 high pulses during vblank (apparently unconnected).^[raw/docs/taito-f3/pal7.txt]
- FDP `121` (from PAL7.19) goes low for 232 pixels at the end of vsync (end of the last long low pulse). What the FDP does with it is unknown; 12Me21 first guessed it was the "start rendering sprites" signal, but sprite rendering is understood to start around scanline 255, not ~6.^[raw/docs/taito-f3/pal7.txt]
- Hypothesis: PAL7 counts hsync pulses from the falling edge of the frame pulse to derive the 232 px pulse (the notes say "presumably ... counting some number of hsync pulses").^[raw/docs/taito-f3/pal7.txt]
- MAME has no frame pulse; it uses the generic screen vblank (see [[hardware/interrupts]]).^[raw/emu-source/mame-0.289/taito_f3.cpp]

## Sync generator model (startup observations)

12Me21's description: the sync generator has a few flags that only change on the **rising edge of hsync**: (1) vblank (inverts the signal), (2) small/large (432-px line vs 216-px half line), (3) an unknown flag that produces the special 32/184 half scanline.^[raw/docs/taito-f3/vtiming_startup.txt]

Named pieces used in `vtiming_startup.txt` (widths in px):

- core_vsync 2608 px (11 half-lines of 16/200); vsync_A 2824 px; vsync_B 3040 px (ends with `32 400`).^[raw/docs/taito-f3/vtiming_startup.txt]
- special_half_scanline 216 px (`32 184`), short_full_scanline 416 px (`16 400`), long_half_scanline 232 px (`32 200`).^[raw/docs/taito-f3/vtiming_startup.txt]
- Normal frame ("frame type D"): short full scanline, 251 full scanlines (`32 400`), special half scanline, vsync_A, then six `16 200` half-lines before the next frame; the earliest point a normal frame can begin is after three further `16 200` pulses. The "color signals" listing marks a small blip of colour, a first and a last line with colour among the full scanlines.^[raw/docs/taito-f3/vtiming_startup.txt]

## Startup, blank bit, drop-in / drop-out

- At power-up the F3 outputs an invalid video signal (mostly garbled vblanks) for several frames. `vtiming_startup_1.txt` shows ~15 such frames of `16 400, 32 400, 32 184|400, 32 200, 5×16 200, …` before the normal frame starts; `vtiming_startup_2.txt` is the same capture in measured (non-rounded) pixels.^[raw/docs/taito-f3/website/video.html] ^[raw/docs/taito-f3/vtiming_startup_1.txt] ^[raw/docs/taito-f3/vtiming_startup_2.txt]
- The FDP settings register at `$66001E` has a "blank video" bit (`b`, 0x0002): no colour signals, and an invalid sync signal; bit 0 locks the register until reset. The bit layout is 12Me21's, written `[???? ???? e??? ??bl]`.^[raw/docs/taito-f3/scroll-regs.txt]
- Drop-out: writing `$0002` to `$66001E` after a normal frame makes the output switch, after one last vsync, to repeating cycles of 7128 px (16.5 scanlines): `16 400`, 13×`32 400`, `32 200` (like the start of vblank), 4×`16 200`, with **no vsync pulse** — the same kind of garbled frame as at power-up.^[raw/docs/taito-f3/vtiming_dropout_1.txt]
- Drop-in: writing `$0000` to `$66001E` after it was `$0002` produces the same "bad frame" many times, then a normal coloured frame.^[raw/docs/taito-f3/vtiming_dropin_1.txt]
- Hypothesis: the garbled startup frames are the video output with the blank bit still set from reset; the notes do not state this directly.
- See [[hardware/fdp]] for the register block at `0x00660000`.

## Open questions

- Why `vtiming2.txt` was captured "without cartridge" and whether cartridge-less timing differs from the table above; the notes only record the pulse lists.^[raw/docs/taito-f3/vtiming2.txt]
- The unknown third generator flag (special 32/184 half-line) has no known software control.^[raw/docs/taito-f3/vtiming_startup.txt]
- Related quirk: [[quirks/video-blank-bit-invalid-sync]].
