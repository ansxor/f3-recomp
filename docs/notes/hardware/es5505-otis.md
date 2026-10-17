---
title: ES5505 (OTIS) register model and sample banking, as implemented by this runtime
created: 2026-10-11
updated: 2026-10-15
type: hardware
tags: [audio, memory-map, undocumented]
sources:
  - raw/docs/runtime-audio-es5505.md
  - raw/docs/landmakrj-config-excerpts.toml.txt
  - raw/docs/taito-f3/otis-bank.txt
  - raw/emu-source/mame-0.289/taito_en.cpp
  - raw/docs/runtime-audio-sequences.md
  - raw/docs/taito-f3/5505-5510.txt
  - raw/docs/taito-f3/clocks.txt
  - raw/docs/taito-f3/list.txt
games: [landmakrj]
addresses: [0x00200000, 0x00300000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/runtime-audio-es5505.md, note: "runtime documentation of the MAME-derived ES5505 model; not a board measurement"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_en.cpp, note: "MAME bank register and clock wiring"}
  - {kind: doc, ref: raw/docs/taito-f3/5505-5510.txt, note: "12's oscilloscope signal tracing: Host Serial Control register $63 (Sony slave mode), serial protocol sharing BCLK/WCLK/LRCLK, OTIS channels 1..3 via FCM to ESP 0..2, and 128 BCLK LRCLK timing"}
  - {kind: doc, ref: raw/docs/taito-f3/clocks.txt, note: "audio BCLK 3.8095225 MHz and sample rate ~29.76189453 kHz (BCLK/128)"}
contradictions:
  - "Bank mechanism: otis-bank.txt (12Me21) reads banks out of a dual-port RAM through a counter driven by OTIS E/BS; the runtime and MAME model a per-voice bank table written by the sound CPU at 0x300000. Both agree on the CPU-side address; the board-level readout path is not modelled."
supersedes: []
---

# ES5505 (OTIS) register model and sample banking

This page records how this runtime models the ES5505 sample player, as visible to the sound 68000. The
model is a MAME-derived core; matching MAME output is compatibility evidence, not a measurement of the
chip or of the board's analog output. ^[raw/docs/runtime-audio-es5505.md] It does not override any
board observation; the confirmed OTIS to ESP routing is on [[hardware/sound]]. Board context:
[[hardware/board]], [[hardware/cartridge]] (OTIS sample bus).

## Bus view

- 16 registers of 16 bits at `0x200000 + 2*register` on the sound CPU bus. Register `0xf` is `PAGE`:
  `0x00..0x1f` shows the control registers of voice `PAGE & 0x1f`, `0x20..0x3f` the filter memory of that
  voice, `0x40+` a test page. ^[raw/docs/runtime-audio-es5505.md]
- Low-page registers: `CR`, `FC`, `START`/`END` (hi, lo), `K2`, `K1`, `LVOL`, `RVOL`, `ACC` (hi, lo),
  `ACT` (active voices minus 1), `IRQV`. `FC` bit 0, the low five bits of `START`/`END` low, and the low
  four bits of `K1`/`K2` are ignored; only the high byte of the volume words is used. ^[raw/docs/runtime-audio-es5505.md]
- The accumulator is a word address with 9 fraction bits (20 integer bits); the step per output sample is
  `FC / 1024` words, so `FC = 0x400` plays one ROM word per output sample. ^[raw/docs/runtime-audio-es5505.md]
- Output: 32 voices, four stereo pairs selected by `CR` bits 8-9 (`CA`); sample words are linearly
  interpolated, passed through a four-pole filter selected by `CR` bits 10-11, then scaled by two 8-bit
  exponent/mantissa volumes. ^[raw/docs/runtime-audio-es5505.md]
- `CR` bits: 0-1 stop, 2 `LEI`, 3 loop enable, 4 bidirectional loop, 5 IRQ enable, 6 direction, 7 IRQ
  pending, 13 `CMPD`, 14-15 bank select. The model returns the upper four `CR` bits as 1 and does not
  decode `CMPD` (compressed samples); bank selection comes from the board registers below.
  ^[raw/docs/runtime-audio-es5505.md]
- A read of the high-page filter register `O1(n-1)` of a stopped voice returns the raw sample word at the
  voice address; the source comment ties this to Taito F3 games. ^[raw/docs/runtime-audio-es5505.md]
- The voice interrupt (`IRQV`) is not connected to the sound CPU in the runtime; the only sound-CPU
  interrupt modelled is the DUART's level 6 ([[hardware/duart-and-volume]]). ^[raw/docs/runtime-audio-es5505.md]

## Sample banking

- The chip has a 20-bit word address; the board adds a bank register per voice at `0x300000 + 2*voice`.
  The model keeps `bank = (value & mask) << 20` and reads ROM word `(bank + word_addr) & (rom_words - 1)`,
  where `mask = rom_bytes / 0x200000 - 1`. For Land Maker's 16 MiB sample region the mask is 7.
  ^[raw/docs/runtime-audio-es5505.md]
- The driver writes the board bank at `0x300001 + 2*voice` from routine `0xc180b2` after shifting the
  voice's sample-bank selector right once and placing the old low bit at bit 28 of the OTIS address
  (`0xc1807e`). ^[raw/docs/runtime-audio-sequences.md]
- Land Maker sample region layout (config): three 2 MiB chips fill the 16 MiB region at `0x400000`
  (`e61-04.38`), `0x800000` (`e61-05.39`) and `0xc00000` (`e61-06.40`), stride 2; the region below
  `0x400000` stays zero. ^[raw/docs/landmakrj-config-excerpts.toml.txt]
  The runtime documentation says the chips fill the even bytes (the high byte of a big-endian word) with
  the odd bytes zero. ^[raw/docs/runtime-audio-es5505.md]
- Compare [[hardware/sound]] for 12Me21's description of the bank counter hardware and MAME's
  `en_es5505_bank_w`. ^[raw/docs/taito-f3/otis-bank.txt] ^[raw/emu-source/mame-0.289/taito_en.cpp]

## Clocks and reset in the model

- Default master clock 15,238,090 Hz and 32 voices give an output rate of `15238090 / 512` = 29761 Hz
  (the F3 pump rate in [[hardware/sound]]). ^[raw/docs/runtime-audio-es5505.md]
- Chip reset stops every voice (`CR` stop bits, volumes `0x80`, `ACT` = 31); the runtime's board reset
  deliberately does not reset OTIS voices. ^[raw/docs/runtime-audio-es5505.md]

## Serial audio interface and hardware interconnect

- **Host Serial Control register:** Initialized to `$63` `[01100011]`: Sony slave mode, SER3 configured as output, SER0–2 configured as inputs. ^[raw/docs/taito-f3/5505-5510.txt]
- **Clocks:** Three audio serial clocks are shared across all channels: BCLK (3.8095225 MHz on otis.12), WCLK (otis.10), and LRCLK (otis.11). BCLK is 15.23809 MHz / 4. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/clocks.txt; raw/docs/taito-f3/list.txt]
- **LRCLK timing & rate:** LRCLK cycle length is 128 BCLK cycles: 16 bits high (left sample) and 112 bits low (right sample, with only the first 16 bits containing active data). At 3.8095225 MHz BCLK, the resulting audio sample rate is `3809522.5 / 128` ≈ 29.76189453 kHz. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/clocks.txt]
- **Physical interconnect:** OTIS channel 0 (ser0, pin 6) is unused (NC). Channel 1 (ser1, pin 7), channel 2 (ser2, pin 8), and channel 3 (ser3, pin 9) connect through the TC0660FCM (which replaces a 74x174 4x D flip-flop clocked by BCLK) to ESP serial inputs 0, 1, and 2 (pins 15, 14, 13). ^[raw/docs/taito-f3/5505-5510.txt]

Unverified here: none of the `FC`, filter-coefficient or envelope behaviour above was checked against
silicon. Next: [[hardware/es5510-esp]].
