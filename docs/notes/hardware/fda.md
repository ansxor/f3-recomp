---
title: TC0650FDA (color RAM and blender)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, palette, blend, pcb]
sources: [raw/docs/taito-f3/website/fda.html, raw/docs/taito-f3/fda.txt, raw/docs/taito-f3/probe-fda.txt, raw/docs/taito-f3/website/fdp/blend.html, raw/docs/taito-f3/website/fdp/alpha.html, raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/chips.txt, raw/docs/taito-f3/draw.txt, raw/docs/taito-f3/fdp.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp, raw/emu-source/mame-0.289/taito_f3.cpp]
games: []
addresses: [0x00440000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fda.html, note: "role, pinout and wiring notes from PCB tracing"}
  - {kind: doc, ref: raw/docs/taito-f3/probe-fda.txt, note: "oscilloscope probing of FDA input pins and vias"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/blend.html, note: "fda mode / blur bits of line register 2.2"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "palette write handler and final per-pixel blend arithmetic"}
contradictions:
  - "Blur polarity: website/fdp/blend.html says line register 2.2 blur field 0,1 = blur disabled and 2,3 = enabled; line-ram.txt and MAME's comment say bit 13 = 0 enables blur, 1 disables. website/fdp/lineram.html (same site) also says 0,1 blurred / 2,3 normal."
  - "Legacy color mode: 12Me21 says FDA mode 0 is a 15-bit RGB format; MAME's comment on 0x6400 bit 14 says 'interpret palette entries as 12-bit RGB' (unemulated; hard-coded per game)."
supersedes: []
---

# TC0650FDA (color RAM and blender)

The FDA ("F3 Digital to Analog") is the last stage of the video pipeline. For each screen pixel it
reads two RGB colors from color RAM (addressed by the [[hardware/fdp]]), gets a 4-bit alpha value and
a few control bits from the FDP, blends the two colors and outputs analog RGB. ^[raw/docs/taito-f3/website/fda.html]
It is a 100-pin QFP custom chip (IC41), next to three 8K×8 SRAMs (IC38–IC40) that are the palette RAM.
^[raw/docs/taito-f3/chips.txt] ^[raw/docs/taito-f3/draw.txt]

The blending model (priority cells, alpha selection) is on [[hardware/priority-and-blend]]; the
FDP-side registers are in [[hardware/line-ram]]. Board context: [[hardware/board]].

## Palette (color) RAM as the CPU sees it

- The FDA also lets the CPU read and write color RAM, "with help from the fdp" (the author assumes
  the CPU data bus is connected to color RAM data when the FDP or FCM instructs it). ^[raw/docs/taito-f3/website/fda.html]
- Color RAM is three 8-bit chips (labelled 1/IC38, 2/IC39, 3/IC40 in the notes; which colour byte lives
  in which chip is not remembered). The FDP drives all three chips' address pins in common with 13
  address lines, giving a 13-bit color id (0–8191). ^[raw/docs/taito-f3/website/fda.html] ^[raw/docs/taito-f3/fdp.txt]
- MAME maps palette RAM at `0x440000–0x447fff` (8192 32-bit words), with a write handler that converts each
  word to a host pen. ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- MAME's default conversion treats the word as `0x00RRGGBB` (8 bits per channel). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- For four games (spcinvdx, ridingf, arabianm, ringrage) MAME instead reads `RRRR GGGG BBBB ....` from
  bits 15–4, each nibble ×16, with the comment that these 12-bit-palette games are probably selected
  per line by line register 2.2 ("6400") but this is a TODO. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
  Hypothesis: the top bits of 2.2 (`fda` field, see below) are that selector; neither source confirms it.

## Inputs from the FDP

Seven FDP outputs go to the FDA: FDP pins 196–199 → FDA 3–6 (alpha0..alpha3), FDP 202 → FDA 7 (blur),
FDP 203/204 → FDA 95/96 (mode0/mode1), and FDP 181 → FDA 98 (unknown, "cpu?"). ^[raw/docs/taito-f3/fdp.txt] ^[raw/docs/taito-f3/website/fda.html]

Scope probing of these pins (secondhand): ^[raw/docs/taito-f3/probe-fda.txt]

- fdp.204 high; fdp.203 "alpha/enable?", high during non-visible parts; ^[raw/docs/taito-f3/probe-fda.txt]
- fdp.202: during visible parts it is low when blur=1 and high when blur=0 (note's wording); ^[raw/docs/taito-f3/probe-fda.txt]
- fdp.198 cycles high/low every half pixel and is low during non-visible parts; ^[raw/docs/taito-f3/probe-fda.txt]
- fdp.196/197/199 low during the test; ^[raw/docs/taito-f3/probe-fda.txt]
- the author concludes these must be "the 4-bit alpha, 2-bit mode, 1-bit blur control". ^[raw/docs/taito-f3/probe-fda.txt]

## Mode, blur and alpha bits (software side)

Line register 2.2 ("6400") and 2.1 ("6200") feed these signals. ^[raw/docs/taito-f3/website/fdp/blend.html]

| Field | Meaning | Source |
|---|---|---|
| `fda` (bits 15–14 of 2.2) | 0 = legacy color mode (15-bit RGB); 1 = normal; 2 = no blending, half-pixels displayed separately ("debug mode?"); 3 = invalid?, first half-pixel color repeats across the pixel (alpha still works) | ^[raw/docs/taito-f3/website/fdp/blend.html] |
| `blur` (bits 13–12 of 2.2) | "In practice" 0,1 = blur disabled, 2,3 = enabled (website). Technically an FDA input that goes low during the selected visible half-pixels: 0 never, 1 during A, 2 during B, 3 during both. Level during half-pixel A "doesn't seem to matter". In modes 2 and 3 the level for B is sent for both. | ^[raw/docs/taito-f3/website/fdp/blend.html] |
| blur polarity (alt.) | line-ram.txt: "B - horizontal blur (FDA): 0 = enable, 1 = disable"; MAME comment: "0 = enable horizontal forward blur (1 = don't blur)" | ^[raw/docs/taito-f3/line-ram.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] |
| alpha values (2.1 "6200") | four 4-bit values, `alpha = clamp((15 − value)/8, 0, 1)`; $0–$7 = 100 %, $B = 50 %, $F = 0 % | ^[raw/docs/taito-f3/website/fdp/alpha.html] |

Modes 2 and 3 are not used by any game and "may have various strange effects that we haven't tested
much"; the author also suspects the FDA mode affects some FDP behaviour. ^[raw/docs/taito-f3/website/fdp/blend.html]
"Alpha functions normally in all modes (multiplying the brightness)." ^[raw/docs/taito-f3/line-ram.txt]

## How the two half-pixels are combined

- Per screen pixel the FDA receives two colors, "half-pixel" A and B, each with its own alpha. The
  half-pixel clock is 13.343 MHz (FDA pin 92); the pixel clock is half that, 6.6715 MHz, supplied
  inverted through jumper JP2 (pin 93). ^[raw/docs/taito-f3/website/fda.html]
- The exact analog blend equation (e.g. whether alpha_A·A + alpha_B·B, and where saturation occurs)
  is not written down in the 12Me21 notes. Hypothesis: the output is the sum of each half-pixel color
  scaled by its alpha, clamped at full scale; this is what MAME does (next section).
- MAME's model: for every pixel it has a `src` and `dst` palette index with contributions `src_blend`
  and `dst_blend` in 0..8 (8 = 100 %); output per channel is `min(255, (src·src_blend + dst·dst_blend) >> 3)`. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
  MAME converts alpha as `min(8, 0xf − value)`, which agrees with the 12Me21 formula. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- MAME does not emulate FDA mode, blur, or the legacy 15-bit mode (the comment marks them unemulated). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Pinout summary

The FDA has 100 pins. Groups (full table with the author's "?" marks: [raw](../raw/docs/taito-f3/website/fda.html),
[plain list](../raw/docs/taito-f3/fda.txt)): ^[raw/docs/taito-f3/website/fda.html] ^[raw/docs/taito-f3/fda.txt]

- 3–7: alpha0–3 and blur from the FDP; 95–96: mode0/mode1 from the FDP; 98: FDP pin 181; ^[raw/docs/taito-f3/fda.txt]
- 8–14, 17–25: CPU data bits D16–D31 (pin 8 = D16 … pin 25 = D31, with 15–16 power); 29: CPU R/W; ^[raw/docs/taito-f3/fda.txt]
- 26–28: inputs probably from the FCM (pins 99/97/98 of the FCM, with "?"); ^[raw/docs/taito-f3/fda.txt]
- 53–78: data pins to the three color RAM chips (mostly "?"); 85: color RAM output enable; 86/87/88: write
  enables for color RAM 3/2/1; 89: reset; ^[raw/docs/taito-f3/fda.txt] ^[raw/docs/taito-f3/website/fda.html]
- 37, 39, 44: blue, green, red color outputs to the DACs, each with a 0.35 V reference divider (R15 75 Ω,
  R16 1000 Ω, R17 75 Ω: 75/(1000+75)·5 V ≈ 0.35 V). ^[raw/docs/taito-f3/fda.txt]
- Vias under the chip carry the blanking-time spikes and signals recorded in the probe notes
  ([raw](../raw/docs/taito-f3/probe-fda.txt)); C5/C6 are thought to be audio-related. ^[raw/docs/taito-f3/probe-fda.txt]

## Open points

- Which CPU data bits reach which color RAM chip, and the real palette word layout on the bus, are
  not determined by these notes. ^[raw/docs/taito-f3/website/fda.html]
- Behaviour of FDA modes 2/3 and of the "legacy" mode is untested/unemulated (see the contradiction above). ^[raw/docs/taito-f3/website/fdp/lineram.html] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
