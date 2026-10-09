---
title: F3 oscillators and clock distribution
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [pcb, timing, cpu, video, audio]
sources: [raw/docs/taito-f3/list.txt, raw/docs/taito-f3/mbv.txt, raw/docs/taito-f3/website/cart.html, raw/docs/taito-f3/pal9.txt, raw/docs/taito-f3/fcm.txt, raw/emu-source/mame-0.289/taito_f3.cpp, raw/emu-source/mame-0.289/taito_f3.h, raw/emu-source/mame-0.289/taito_en.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/list.txt, note: "traced clock nets: oscillator outputs, dividers, buffers"}
  - {kind: doc, ref: raw/docs/taito-f3/website/cart.html, note: "clock pins on the cartridge connector with figures"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "MAME screen clock 26.686 MHz / 4 and CPU clock constants"}
contradictions:
  - "Main CPU clock: MAME's machine config uses F3_MAIN_CLK = 16 MHz (taito_f3.h) while MAME's own board comment and 12Me21's cartridge notes give 15.23809 MHz (30.47618 MHz / 2). The notes route the 16 MHz oscillator only to clkbuf and the 5701, not to the CPU."
  - "ES5510 clock: 12Me21's list.txt and MAME's board comment say 8 MHz; taito_en.cpp constructs ES5510 from a 10 MHz XTAL (comment: 'from Gun Buster schematics')."
  - "list.txt calls the OTIS E output '3.8MHz ... input divided by 8'; 30.47618/8 = 3.8095 but the OTIS CLKIN is the 15.238 MHz net, so the divisor given and the input named do not agree (arithmetic only; not resolved by the sources)."
supersedes: []
---

# F3 oscillators and clock distribution

Three can oscillators sit on the board. Figures are exactly as the sources print them; `?` marks the notes' own doubt. For where each clock goes see [[hardware/board]]; the video side is in [[hardware/video-timing]].

## Oscillators

| Ref (New / Single) | Frequency | Goes to |
|---|---|---|
| x1 / x3 | 26.686 MHz | vclkdiv (ic19) pin 11 ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/list.txt] |
| x2 / x2 | 30.47618 MHz | clkbuf (ic20) pin 4 → buffered pin 16 → 5701 pin 19 and FCM pin 131 ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/list.txt] |
| x3 / x1 | 16 MHz | clkbuf pin 2 → buffered pin 18 → 5701 pin 10 ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/list.txt] |

The oscillator part is a KXO-01 TTL clock oscillator. ^[raw/docs/taito-f3/chips.txt]

## Video clocks (from the 26.686 MHz oscillator)

ic19 (74F74, "vclkdiv") divides the 26.686 MHz oscillator twice by two, with feedback wires tying Q̄ to D. ^[raw/docs/taito-f3/list.txt]

- **13 MHz** (13.343 MHz per the cartridge page): ic19 pin 3/9 → clkbuf pins 6/13 → buffered a (clkbuf.14 → R52 → FDA pin 92 and FDP pin 124) and buffered b (clkbuf.7 → R54 → cartridge C01). ^[raw/docs/taito-f3/list.txt] ^[raw/docs/taito-f3/website/cart.html]
- **7 MHz** (6.6715 MHz): ic19 pin 5 → clkbuf pins 8/11 → buffered a (clkbuf.12 → R53 → jp2.1, both graphics RAM OE pins, FDP pin 123) and buffered b (clkbuf.9 → R55 → cartridge C02). ^[raw/docs/taito-f3/list.txt]
- FDA pin 93 is fed through a 3-pin jumper (jp2 in list.txt; mbv.txt names it jp1, Single jp6, "fda 7mhz clock input select"): jp2.1 is the buffered 7 MHz net, jp2.3 is the ic19 pin 2/6 feedback net (the divider's own D/Q̄ loop), jp2.2 goes to FDA pin 93. Hypothesis: the jumper chooses between the 7 MHz clock and its inverse (Q̄ of the last divider stage). ^[raw/docs/taito-f3/list.txt] ^[raw/docs/taito-f3/mbv.txt]
- **3.33575 MHz** on cartridge C03 ("2 pixels"): the notes say C03/A89 connects to fdp.180 and fdp.207 and is "3mhz clock, disabled during reset or something?". The source of this clock is not identified. ^[raw/docs/taito-f3/website/cart.html] ^[raw/docs/taito-f3/list.txt] ^[raw/docs/taito-f3/cart.txt]
- Phase relationship: C01 is high during the first half of each C02 cycle, and likewise C02 for C03. ^[raw/docs/taito-f3/website/cart.html]

### Pixel clock

The 7 MHz tap (26.686 / 4 = 6.6715 MHz) is MAME's pixel clock: `set_raw(26.686_MHz_XTAL / 4, 432, 46, 320+46, 262, 24, 232+24)`, with the comment "refresh rate = 26686000/4/432/262 = 58.94 Hz", citing the Taito Z system crystal and measurements from arcade-projects. ^[raw/emu-source/mame-0.289/taito_f3.cpp] The 13.343 MHz tap is therefore a half-pixel clock (two clocks per pixel) and the 3.33575 MHz clock is a two-pixel clock; the pixel/half-pixel naming is [INFERENCE] from the frequencies, though the cartridge notes do call C03 "2 pixels". ^[raw/docs/taito-f3/website/cart.html] The sprite/tile ROM PAL (pal9) calls its 3.33 MHz input "2 pixels" and uses it, together with the 13 and 7 MHz inputs, to sequence latch-enable and output-enable for the ROM buses. ^[raw/docs/taito-f3/pal9.txt] See [[hardware/pal-decoders]].

## CPU / audio clock (30.47618 MHz side)

- A 15.238 MHz net ("15mhz clock") feeds: ic1 pin 10 (clock driver; ic1.8 buffered → R2 → main CPU pin 5 CLK), cartridge A82 and D69, the audio CPU CLK, 5701 pin 21, OTIS CLKIN pin 34, and pal6 pin 1, with pull-up R1. The notes say "idk who is responsible for generating this? maybe 5701 again" — i.e. the 5701 is hypothesised to divide the 30.47618 MHz it receives on pin 19. ^[raw/docs/taito-f3/list.txt]
- A82 and D69 are the same net: both CPUs use the same clock source, printed as 15.23809 MHz. ^[raw/docs/taito-f3/website/cart.html]
- MAME's board comment: "68020 clock: 15.23809 (30.47618 / 2)", "68000 clock: 15.23809", "5505 clock: 15.23809MHz". ^[raw/emu-source/mame-0.289/taito_f3.cpp] taito_en.cpp builds the audio 68000 and ES5505 at `30.47618_MHz_XTAL / 2`. ^[raw/emu-source/mame-0.289/taito_en.cpp]
- The main CPU in MAME is clocked by `F3_MAIN_CLK = 16_MHz_XTAL`. ^[raw/emu-source/mame-0.289/taito_f3.h] ^[raw/emu-source/mame-0.289/taito_f3.cpp] (contradiction recorded above; effect on a recompiler is only cycle-count scaling of about 5%, [INFERENCE]).

## Audio-side dividers

- 8 MHz "idk esp clock" (ic18 pin 2, 5701 pin 8, ESP pin 1): who generates it is unknown; probably the 5701 from its 16 MHz input. ^[raw/docs/taito-f3/list.txt]
- ic18 (74F163 counter) divides it: pin 14 = ÷2 → 4 MHz → DUART X1/CLK (pin 32); pin 12 = ÷8 → 1 MHz → DUART pins 38/36 and FIO pin 107; pin 11 = ÷16 → 0.5 MHz → DUART pins 2/39; pin 13 (÷4) probably unconnected. ^[raw/docs/taito-f3/list.txt]
- MAME's DUART: `16_MHz_XTAL / 4` (4 MHz) with counters at 16/2/8 and 16/2/16 MHz; the board comment lists 68681 pins 2/32/36/38/39 as 500 kHz/4 MHz/1 MHz/1 MHz/500 kHz, agreeing. ^[raw/emu-source/mame-0.289/taito_en.cpp] ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- MAME's board comment lists ESP clocks: pin 1 8.000 MHz, pins 4–6 2.6686 MHz, pins 9–10 29.7623 kHz, pin 16 3.8095225 MHz. ^[raw/emu-source/mame-0.289/taito_f3.cpp] 
- OTIS `E` output ("3.8MHz otis e", pin 30, "input divided by 8") drives ic21 pin 3 (the voice bank counter clock), pal8 pin 8, pal4.17, ic30, FCM pin 48 and pin 122, 5701 pin 12; low means OTIS is accessing sound memory. ^[raw/docs/taito-f3/list.txt] MAME's sound pump runs at `30.47618 MHz / (2*16*32)` ≈ 29.76 kHz, matching the 29.7623 kHz ESP figure in the board comment. ^[raw/emu-source/mame-0.289/taito_en.cpp] ^[raw/emu-source/mame-0.289/taito_f3.cpp]

## Video sync

clkbuf pin 17/3 buffer FDP pin 120 (video sync) toward R24 and the monitor sync output; "this is what that bodge wire is connected to". ^[raw/docs/taito-f3/list.txt]
