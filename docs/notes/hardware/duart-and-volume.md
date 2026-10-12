---
title: Sound-board DUART timer, ESP halt output and MB87078 volume, as implemented by this runtime
created: 2026-10-11
updated: 2026-10-11
type: hardware
tags: [audio, interrupt, timing, memory-map]
sources:
  - raw/docs/runtime-audio-duart-and-gain.md
  - raw/docs/runtime-audio-sequences.md
  - raw/tests/landmakrj-seed5-sound-trace.md
  - raw/emu-source/landmakrj-boot.cpp.txt
  - raw/docs/taito-f3/duart.txt
  - raw/emu-source/mame-0.289/taito_en.cpp
  - raw/docs/runtime-hle-audio-rom-protocol.md
  - raw/tests/2026-10-10-otis-esp-routing-experiments.md
games: [landmakrj]
addresses: [0x00280000, 0x00340000, 0x00C007F8]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/runtime-audio-duart-and-gain.md, note: "runtime documentation of the MC68681 and MB87078 models and the host mixer"}
  - {kind: trace, ref: raw/tests/landmakrj-seed5-sound-trace.md, note: "IRQ read spacing 16,000 main ticks and gain-request time from the seed-5 cold-boot trace"}
contradictions:
  - "DUART output pins: duart.txt marks OP0-OP7 unused; MAME and the runtime use OP6 as ESPHALT. Already listed on [[hardware/sound]]."
supersedes: []
---

# DUART timer, ESP halt and volume chip

Everything here is the MAME-derived runtime model plus ROM-driver observations. Configured clocks, pin
behaviour and gain stages are not physical-board measurements; successful comparisons establish
compatibility with the reference, not hardware truth. ^[raw/docs/runtime-audio-duart-and-gain.md]
Board wiring and the confirmed routing are on [[hardware/sound]]; the ESP side is [[hardware/es5510-esp]].

## MC68681 DUART

- Bus: sound CPU `0x280000-0x28001f`, odd addresses, register `(address >> 1) & 0x0f`. The serial ports are
  unused for communication; the chip supplies the driver tick (counter/timer), ESP halt (OP6) and the
  interrupt vector (IVR, reset value `0x0f`). ^[raw/docs/runtime-audio-duart-and-gain.md]
- Interrupt: level 6 while `ISR & IMR != 0`; the acknowledge returns IVR. A read of register `0xf`
  (stop-counter command) clears the counter-ready bit, and the ROM handler reads `0x28001f` for that.
  ^[raw/docs/runtime-audio-duart-and-gain.md]
- Timer mode (`ACR` bit 6): `ACR` bits 5-4 pick the source (0 = IP2 1 MHz, 1 = IP2/16, 2 = 4 MHz crystal,
  3 = crystal/16); the counter-ready bit sets every second expiry in the model, so the interrupt period is
  two counter periods. ^[raw/docs/runtime-audio-duart-and-gain.md]
- Land Maker programs counter preset `0x07d0` (2000), `ACR = 0x60` (timer, 4 MHz crystal) and
  `IMR = 0x2b`: 2000 ticks of 4 MHz is 0.5 ms per counter period, so 1 ms (16,000 main ticks) per
  interrupt. The seed-5 trace's modal spacing of actual IRQ reads is 16,000 main ticks.
  ^[raw/docs/runtime-audio-sequences.md] ^[raw/tests/landmakrj-seed5-sound-trace.md]
  The ROM also contains an alternate initialisation with counter `0x09c4` that the observed 32-voice mode
  does not use. ^[raw/docs/runtime-audio-sequences.md]
- Output pins: the model passes `OPR ^ 0xff` to the output callback; OP6 high means the ESP is halted
  (reset state). Writing `0x40` to `0x28001d` (set) releases the ESP, to `0x28001f` (reset) halts it.
  ^[raw/docs/runtime-audio-duart-and-gain.md]
- Serial transmitters exist only because the driver reads their status; nothing is connected to TX, and
  receive data is never delivered. IP reads return `0xbf`, register `0xa` reads `0x61`.
  ^[raw/docs/runtime-audio-duart-and-gain.md] (the `0xbf` and `0x61` constants were also seen in
  `runtime/third_party/audio/mc68681.cpp` on 2026-10-11).

## MB87078 volume controller

- Sound CPU `0x340000-0x340003`, even addresses, with the register offset XOR 1 (as MAME does): address
  `0x340000` is the control latch and `0x340002` the data latch, not left/right volumes. A data write
  commits the setting for the channel selected by the control latch. ^[raw/docs/runtime-audio-duart-and-gain.md]
  The ROM-derived correction (control and data latches, not left/right channels) is also recorded in
  the sound-driver decoding notes. ^[raw/tests/landmakrj-seed5-sound-trace.md]
- Channel latch (9 bits): data (bits 0-5), `EN` (bit 6, 0 mutes), `C0` (bit 7, 0 dB), `C32` (bit 8,
  -32 dB); otherwise attenuation is `-0.5 dB` per step of `~data & 0x3f`. Reset latches are `0x7f`
  (enabled, 0 dB). Only channels 2 and 3 (left, right) reach the mix in the runtime.
  ^[raw/docs/runtime-audio-duart-and-gain.md]

## Host mixer gain (runtime implementation, not a board claim)

- The default gain model applies the volume twice (before the ESP input at `0.18 * route`, and after the
  ESP at `route`), where `route` is the modelled gain as a percentage divided by 32 (3.125 at 0 dB).
  The alternative `SingleStage` model is an unverified experiment. A fixed host output boost of 12
  (+21.6 dB) is applied after the DSP because the MAME-equivalent mix peaks near -25 dBFS. The source
  lines `0.18f` and `output_boost` were seen in `runtime/audio/audio.cpp` on 2026-10-11.
  ^[raw/docs/runtime-audio-duart-and-gain.md]
- Changing OTIS to ESP gain from 0.18 to 1.0 raised Command War RMS by 5.55x in a test; whether the
  physical board carries unity-gain words is unestablished (see [[hardware/sound]]). ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

## When the game sets the master volume

- Main code at `$3400/$3404/$340a/$3410` writes four gain requests to the shared RAM at main
  `$c007f8..$c007fb` at about 13.2338 s in the seed-5 cold-boot trace (the observed values were `$30`);
  freezing at 660 frames kept the start-up attenuation, and the default 900-frame boot
  (15.268770 s) is past it. ^[raw/tests/landmakrj-seed5-sound-trace.md] ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- Hypothesis: 13.2338 s is 780 frames at the runtime's 6,671,500 Hz / (432 x 262) frame rate (computed
  here: 780.05; the same rate gives 900 frames = 15.26877 s, matching the recorded event origin). Boot
  completion is observed at frame 781 for a blank EEPROM, so the gain write plausibly belongs to the
  cold-boot path; the two are not tied by any measurement here. With an initialised EEPROM boot completes
  at frame 478, so the gain write would come earlier. See [[games/landmakrj]] boot section.
  ^[raw/emu-source/landmakrj-boot.cpp.txt]
- The runtime documentation's statement that the game "sets its output gain at about 13.23 seconds (780
  frames)" is the arithmetic above plus the trace timing; `games/landmakrj/boot/boot.cpp` measures boot
  completion, not gain. ^[raw/emu-source/landmakrj-boot.cpp.txt]

Related: [[hardware/interrupts]] (main-CPU side), [[routines/landmakrj-00c10da0-sound-timer-irq]].
