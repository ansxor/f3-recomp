---
title: ES5510 (ESP) host interface, instruction format and DUART halt, as implemented by this runtime
created: 2026-10-11
updated: 2026-10-15
type: hardware
tags: [audio, memory-map, undocumented]
sources:
  - raw/docs/runtime-audio-es5510.md
  - raw/docs/runtime-hle-audio-rom-protocol.md
  - raw/docs/runtime-audio-sequences.md
  - raw/docs/runtime-audio-duart-and-gain.md
  - raw/docs/otis-esp-dac-routing-2026-10-09.txt
  - raw/emu-source/mame-0.289/taito_en.cpp
  - raw/disasm/landmakrj-sound-driver-excerpts.md
  - raw/docs/taito-f3/5505-5510.txt
  - raw/docs/taito-f3/clocks.txt
  - raw/docs/taito-f3/chips.txt
  - raw/docs/taito-f3/list.txt
games: [landmakrj]
addresses: [0x00240000, 0x00260000, 0x00C0F430, 0x00C0F482, 0x00C0F4BC, 0x00C1C2D2]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/runtime-audio-es5510.md, note: "runtime documentation of the MAME-derived ES5510 port (Christian Brunschen's core); pipeline and arithmetic are implementation behaviour, not silicon measurements"}
  - {kind: doc, ref: raw/docs/runtime-hle-audio-rom-protocol.md, note: "effects inventory read from the Land Maker sound ROM"}
  - {kind: doc, ref: raw/disasm/landmakrj-sound-driver-excerpts.md, note: "ROM disassembly of DSP worker queue post at $c0f430 to $fb28, effects configuration pointer calculation at $c0f482, and configuration worker routine $c0f4bc"}
  - {kind: doc, ref: raw/docs/taito-f3/5505-5510.txt, note: "12's oscilloscope signal tracing: SERMODE $07F8, serial inputs 0..2 from OTIS 1..3 via FCM, serial output 3 via FCM to TDA1543 DAC, and FCM 74x174/74x373 latch replacement"}
  - {kind: doc, ref: raw/docs/taito-f3/clocks.txt, note: "ESP master clock 8 MHz, audio serial bit clock 3.8095225 MHz, sample rate ~29.76189453 kHz"}
contradictions:
  - "Register window: sound-address.txt decodes 0x240000 (see [[hardware/sound]]); the runtime and MAME use 0x260000-0x2601ff. Same chip select; no real disagreement."
supersedes: []
---

# ES5510 (ESP) as modelled by this runtime

The ESP is the effects DSP behind the OTIS. Everything here describes the MAME-derived software model;
it is not an independent measurement of silicon. ^[raw/docs/runtime-audio-es5510.md] How the OTIS channels
reach the ESP inputs and DAC on the board is **confirmed hardware routing from 12's board observation**
and lives on [[hardware/sound]] and [[comparisons/hardware-vs-mame-audio-routing]]; nothing below argues
with it. The runtime's Reference backend still feeds the ESP the MAME way, and HLE follows the
hardware routing (OTIS Pair 1 -> ESP ch 0 Dry, Pair 2 -> ESP ch 1 Delay, Pair 3 -> ESP ch 2 Reverb, Pair 0 -> NC). ^[raw/docs/runtime-audio-es5510.md] ^[raw/docs/taito-f3/5505-5510.txt]

## Host interface (sound CPU)

- Window `0x260000-0x2601ff`, odd addresses only; register number `(address >> 1) & 0xff`.
  ^[raw/docs/runtime-audio-es5510.md]
- `0x00-0x02` GPR latch (24 bit, high byte first), `0x03-0x08` instruction latch (48 bit),
  `0x09-0x0b` data-in latch (DIL), `0x0c-0x0e` data-out latch (DOL), `0x0f-0x11` delay-RAM address latch
  (a write to `0x0f` triggers the RAM access), `0x12` host control (bit 1 clears delay RAM while halted),
  `0x14` RAM select (bit 7: read), `0x1f` halt enable. `0x80` read-select copies instruction and register
  into the latches; `0xa0`/`0xc0`/`0xe0` commit the GPR latch, the instruction latch, or both.
  ^[raw/docs/runtime-audio-es5510.md]
- The driver loads a program by writing six instruction bytes then the select register `0xc0` with the
  instruction number, repeating per instruction; registers load the same way through `0xa0`.
  ^[raw/docs/runtime-audio-es5510.md]
- Effects configuration load: routines `0xc19982`/`0xc19998` write typed blocks through these host commits;
  the effects worker is posted to task queue `$fb28` by `$c0f430` (TRAP #9) and selected/dispatched by
  routine `$c0f4bc`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]

## State and instructions

- 192 GPRs of 24 bit, 160 instructions of 48 bit, a 1,048,576 x 16-bit delay RAM, serial ports
  `ser0l..ser3r` (registers 234-241, 16-bit sample times 256), a 48-bit MAC register, flags N/C/V/LT/Z.
  ^[raw/docs/runtime-audio-es5510.md]
- Instruction fields: bits 3-5 RAM control, 6 accumulate, 7 skippable, 8-11 operand select, 12-15 ALU op
  (`ADD SUB ADDU SUBU CMP AND OR XOR ABS MOV ASL2 ASL8 LS15 DIFF ASR END`), then four 8-bit register
  fields (ALU A, ALU B, multiplier C, multiplier D). ^[raw/docs/runtime-audio-es5510.md]
- The model is pipelined; `END` decrements `dbase` by `memincrement` (wrapping to `dlength`) and a
  run is bounded by a 200-pass safety counter. ^[raw/docs/runtime-audio-es5510.md]
- In the runtime the ESP runs once per output sample only while not halted. ^[raw/docs/runtime-audio-es5510.md]

## Halt line

The DUART output pin OP6 drives ESP halt: after reset the pins read high and the ESP is halted; the
driver clears the halt by writing `0x40` to the DUART output-set register (`0x28001d`) and halts it with
the output-reset register (`0x28001f`). ^[raw/docs/runtime-audio-duart-and-gain.md] Note the existing
contradiction on [[hardware/sound]]: 12Me21's `duart.txt` marks OP0-OP7 as unused, while MAME and this
model use OP6 as ESPHALT. See [[hardware/duart-and-volume]]. ^[raw/emu-source/mame-0.289/taito_en.cpp]

## Serial audio interface and board interconnect

- **SERMODE register:** Initialized to `$07F8` `[0000011111111000]` ("normal"). ^[raw/docs/taito-f3/5505-5510.txt]
- **Serial channels & routing:** ESP serial port 0 (serio0, pin 15) inputs from OTIS channel 1; serial port 1 (serio1, pin 14) inputs from OTIS channel 2; serial port 2 (serio2, pin 13) inputs from OTIS channel 3; serial port 3 (serio3, pin 12) outputs serial audio to DAC pin 3 (data). All four lines pass through the TC0660FCM (which replaces a 74x174 4x D flip-flop clocked by BCLK). ^[raw/docs/taito-f3/5505-5510.txt]
- **Clocks:** ESP CLKIN (pin 1) runs at 8 MHz from the `ic18.2 → dr2.1 → esp.1` net. Audio serial clocks shared with OTIS: BCLK (pin 16, 3.8095225 MHz), LRCLK (pin 17, 128 BCLK cycles per frame), and WCLK (pin 18). Audio sample rate is ~29.76189453 kHz (`bclk / 128`). ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/clocks.txt; raw/docs/taito-f3/list.txt]
- **Delay RAM latch:** The TC0660FCM replaces a 74x373 8x transparent latch between the ESP's RA/D pins (pins 62–78) and delay RAM, with ESP GATE (pin 5, fcm.79) driving the latch clock. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/list.txt]
- **Board control nets:** `esp halt` on pin 36 (connected via dr2.7 to pal8.12); `esp cs/as` on pins 45/46; `esp/otis rw` on pin 48 (5701 pin 16); and host data bus `esp hdb0..hdb7` on pins 44..37. ^[raw/docs/taito-f3/list.txt]

## Land Maker effects inventory (ROM-derived, hypothesis)

- Thirteen effect configurations at `0xc1c2d2` (16-byte stride) select eleven ESP programs through the
  pointer table at `0xc1c3a2`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/docs/runtime-hle-audio-rom-protocol.md]
  The apparent contradiction between `$c0f482` and `$c0f4bc` in earlier driver notes is resolved by ROM
  disassembly: `$c0f482` is the arithmetic helper that multiplies configuration index `d0` by 16 (`mulu.w #$10, d0`)
  and calculates pointer `a4 = $c1c2d2 + 16*d0`, while `$c0f4bc` is the higher-level worker routine that calls
  `$c0f440` (which calls `$c0f482`), stores pointer `a4` at `$d78c.w` and ID at `$d790.w`, and dispatches the DSP
  loader. ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
  Mailbox command `0x20` selects a configuration and queues the effects worker to queue `$fb28`; `0x21` stores a
  parameter byte at `0xc84a + 2*parameter`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- A seed-5 6000-frame oracle capture holds one `20 0b` selection (configuration 11 selects ESP program 6 at
  `0xc1d4d4`, instruction CRC `cdcfa577`) and parameter writes 0..9 with values
  30,58,73,32,37,0,40,43,18,32; boot loads configuration 0. This is an observed inventory, not every game
  mode. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- ROM menu callbacks identify parameter 2 as delay-address increments `value * 0x7000`, parameter 6 high
  nibble as damping and low nibble as route balance, parameters 8/9 as complementary wet/dry gains,
  parameter 0 as decay/diffusion and parameter 1 as a signed feedback coefficient. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- The isolated-model measurement of the cross-delay is 8177 samples at 29761 Hz (about 274.8 ms) with
  feedback about 0.45; this measures the MAME-derived model, not the board. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
  The command and driver side of the effects worker is on [[routines/landmakrj-00c12ecc-sound-command-dispatch]].

Open: none of the instruction semantics (overflow, skip flags, I/O cycles) are verified against a real
chip. The existing runtime tests exercised the model, not hardware. ^[raw/docs/runtime-audio-es5510.md]
