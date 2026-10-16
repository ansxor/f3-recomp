---
title: Hardware vs MAME — OTIS/ESP audio routing
created: 2026-10-09
updated: 2026-10-15
type: comparison
tags: [audio, pcb, emulator-bug]
sources:
  - raw/docs/otis-esp-dac-routing-2026-10-09.txt
  - raw/docs/otis-esp-dac-routing-2026-10-09-followup.txt
  - raw/docs/otis-esp-routing-12-remark-2026-10-10.txt
  - raw/docs/taito-f3.md
  - raw/docs/taito-f3/5505-5510.txt
  - raw/docs/taito-f3/chips.txt
  - raw/docs/taito-f3/fcm.txt
  - raw/docs/taito-f3/list.txt
  - raw/emu-source/mame-0.289/taito_en.cpp
  - raw/tests/2026-10-10-otis-esp-routing-experiments.md
  - raw/tests/2026-10-10-enhanced-effects-hardware-routing.md
games: [commandw, landmakrj]
addresses: []
status: confirmed
evidence:
  - {kind: doc, ref: raw/docs/otis-esp-dac-routing-2026-10-09.txt, note: "12's initial board-routing relay (with reversed ESP serio0..3 numbering); historical context"}
  - {kind: doc, ref: raw/docs/otis-esp-routing-12-remark-2026-10-10.txt, note: "user relays 12's initial index-swap remark prior to datasheet pin reconciliation"}
  - {kind: doc, ref: raw/docs/taito-f3/5505-5510.txt, note: "12's oscilloscope signal tracing in submodule commit d95fddf: OTIS ser1..3 via FCM to ESP serio0..2, ESP serio3 to DAC via FCM, Host Serial Control and SERMODE registers, FCM 74x174/74x373 replacement, and 128-BCLK LRCLK timing"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_en.cpp, note: "MAME ES5505-to-pump routes; comparison only, lower-ranked than hardware"}
  - {kind: test, ref: raw/tests/2026-10-10-otis-esp-routing-experiments.md, note: "runtime reference/LLE instrumentation and muting/gain experiments; not hardware routing evidence"}
  - {kind: test, ref: raw/tests/2026-10-10-enhanced-effects-hardware-routing.md, note: "user-reported Enhanced hardware-routing test and 2400-frame Land Maker measurement"}
contradictions:
  - "Earlier 12 relay (otis-esp-dac-routing-2026-10-09.txt) mapped ESP pins 12–15 in reverse order (serio3..serio0), creating an apparent conflict with datasheet pin labels; resolved in submodule commit d95fddf (5505-5510.txt), where oscilloscope comparisons align physical traces with ES5510 datasheet pin labels (pin 15 = serio0, pin 12 = serio3)."
  - "Earlier analysis hypothesized reversed register-to-pin numbering so MAME already matched hardware; hardware evidence refutes that conclusion. Earlier analysis claimed 12's pair swap would break the mix; hardware evidence refutes that claim."
supersedes: []
---

# Hardware vs MAME — OTIS/ESP audio routing

**Confirmed hardware routing:** On real hardware (`5505-5510.txt`), OTIS (ES5505) channel 0 (`ser0`) is unconnected (`nc`); channel 1 (`ser1`) feeds ESP (ES5510) channel 0 (`serio0`, dry pass-through in ROM); channel 2 (`ser2`) feeds ESP channel 1 (`serio1`, delay); channel 3 (`ser3`) feeds ESP channel 2 (`serio2`, reverb); and ESP channel 3 (`serio3`) outputs serial stereo audio to the TDA1543 DAC. 12's board observations are hardware evidence, and the user explicitly states these reportings match hardware; this outranks MAME, datasheet pin labels, and our ESP-microprogram analysis. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/otis-esp-routing-12-remark-2026-10-10.txt]

In `5505-5510.txt` (now ingested from the updated submodule commit `d95fddf`), 12 documents the oscilloscope-verified interconnect:

```
OTIS channels:
0 - unused
1 - output to ESP channel 0
2 - output to ESP channel 1
3 - output to ESP channel 2

ESP channels:
0 - input from OTIS channel 1
1 - input from OTIS channel 2
2 - input from OTIS channel 3
3 - output to DAC

connections determined by comparing signals using oscilloscope:
otis.6 (ser0) → nc?
otis.7 (ser1) → FCM → esp.15 (serio0)
otis.8 (ser2) → FCM → esp.14 (serio1)
otis.9 (ser3) → FCM → esp.13 (serio2)
esp.12 (serio3) → FCM → dac.3 (data)
```
^[raw/docs/taito-f3/5505-5510.txt]

The TC0660FCM replaces a 74x174 4x D flip-flop (outputs to D, inputs to Q, clocked by OTIS BCLK) and a 74x373 8x transparent latch for ESP RAM data/address lines with GATE driving clock. ^[raw/docs/taito-f3/5505-5510.txt]

## MAME routing vs Hardware

In MAME (`taito_en.cpp`), OTIS Pairs 1, 2, and 3 were actually connected to the correct ESP channels (`1->0, 2->1, 3->2`):
- OTIS Pair 1 (`ser1`, channels 2/3) → pump inputs 2/3 (ESP channel 0 `serio0`, dry pass-through in ROM)
- OTIS Pair 2 (`ser2`, channels 4/5) → pump inputs 4/5 (ESP channel 1 `serio1`, delay echo)
- OTIS Pair 3 (`ser3`, channels 6/7) → pump inputs 6/7 (ESP channel 2 `serio2`, reverb)
- ESP channel 3 (`serio3`) → pump outputs 2/3 (final DSP audio to output mixer)

MAME's real audio routing bugs were:
1. **Aux bypass of unconnected Pair 0:** MAME routes OTIS Pair 0 (channels 0/1) to pump inputs 0/1, which bypasses the ESP and mixes directly into the final output. On real hardware (`5505-5510.txt`), OTIS pin 6 (`ser0`, Pair 0) is unconnected (`nc`), so Pair 0 never reaches the DAC.
2. **Double volume scaling:** MAME applies MB87078 gain twice (before ESP inputs and after the pump in `MameRouting`), whereas on hardware the MB87078 provides single-stage post-DAC analog attenuation (`list.txt:265-280`).
3. **No L/R DAC swap:** MAME does not account for the TDA1543 DAC swapping left/right channels due to Sony slave LRCLK polarity vs standard I2S. ^[raw/emu-source/mame-0.289/taito_en.cpp L261-L269; raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/list.txt]

The earlier belief that MAME swapped pairs 1 and 3 arose from the 2026-10-09 preliminary relay (`otis-esp-dac-routing-2026-10-09.txt`), where 12 had numbered ESP pins 12–15 in reverse order (`serio3..serio0`). 12 remarked at that time that this looked like a "second 1↔3 index swap" on F3, but `5505-5510.txt` subsequently corrected the pin numbering against the ES5510 datasheet, establishing that Pair 1 feeds ESP ch 0 and Pair 3 feeds ESP ch 2 without any index swap. ^[raw/docs/otis-esp-routing-12-remark-2026-10-10.txt; raw/docs/taito-f3/5505-5510.txt]

## Contradictions and superseded analysis

In the earlier 2026-10-09 relay (`otis-esp-dac-routing-2026-10-09.txt`), 12 labeled pins 15, 14, 13, 12 as `serio3..serio0`, which inverted channel names relative to the ES5510 datasheet (where pin 15 is `SERI/O0` and pin 12 is `SERI/O3`). In `5505-5510.txt`, 12 corrected the pin-to-channel labels to match the datasheet: OTIS pin 7 (ser1) → ESP pin 15 (serio0, channel 0); OTIS pin 8 (ser2) → ESP pin 14 (serio1, channel 1); OTIS pin 9 (ser3) → ESP pin 13 (serio2, channel 2); ESP pin 12 (serio3, channel 3) → DAC pin 3. Both notes agree on the exact physical PCB pins connected (OTIS 7→ESP 15, OTIS 8→ESP 14, OTIS 9→ESP 13, ESP 12→DAC 3). ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/chips.txt; raw/docs/taito-f3/fcm.txt]

Earlier analysis proposed reversed register↔pin numbering as a way for MAME to already match hardware. This is refuted by the higher-ranked hardware observation. Earlier analysis also claimed 12's swap would break the mix; that claim is likewise refuted by hardware evidence. These earlier inferences remain recorded here as refuted, not erased. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md; raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/docs/otis-esp-routing-12-remark-2026-10-10.txt]

The earlier tentative report had an internally conflicting ESP pin-13 assignment (serio1 and serio0/DAC) and marked OTIS ser0 uncertain. The subsequent oscilloscope tracing in `5505-5510.txt` settles the physical and logical channel routing above; the earlier report remains historical context only. ^[raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/docs/taito-f3/5505-5510.txt]

## Runtime evidence and implications

Runtime instrumentation in Command War and Land Maker found their loaded ESP programs reading SER0–SER2 and writing stereo output only to SER3; pair-muting tests changed output samples in the tested runtime. These are runtime findings, not evidence that overrides the hardware wiring observation. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

HLE (`runtime/audio/hle/effects.hpp`) now maps Pair 1 (`buses 2/3`) to Dry (ESP ch 0), Pair 2 (`4/5`) to Delay (ESP ch 1), Pair 3 (`6/7`) to Reverb (ESP ch 2), and drops Pair 0 (`buses 0/1`). Reference retains MAME's baseline routing for comparison. The runtime default gain model is now `SingleStage` (single-stage post-DAC analog attenuation, matching `list.txt:265-280`). The Enhanced test and 2400-frame Land Maker measurement are recorded in the runtime evidence note. ^[raw/tests/2026-10-10-enhanced-effects-hardware-routing.md; raw/docs/taito-f3/5505-5510.txt]

The runtime gain experiment found changing emulated OTIS→ESP gain from 0.18 to 1.0 raised Command War RMS by 5.55×; this describes the runtime mix only and does not establish physical link gain. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

See [[hardware/sound]] for the board and sound-CPU context, and [[hardware/fcm]] for FCM pin-label context. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3.md]
