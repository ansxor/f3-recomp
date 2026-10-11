---
title: Hardware vs MAME — OTIS/ESP audio routing
created: 2026-10-09
updated: 2026-10-10
type: comparison
tags: [audio, pcb, emulator-bug]
sources:
  - raw/docs/otis-esp-dac-routing-2026-10-09.txt
  - raw/docs/otis-esp-dac-routing-2026-10-09-followup.txt
  - raw/docs/otis-esp-routing-12-remark-2026-10-10.txt
  - raw/docs/taito-f3.md
  - raw/emu-source/mame-0.289/taito_en.cpp
  - raw/tests/2026-10-10-otis-esp-routing-experiments.md
  - raw/tests/2026-10-10-enhanced-effects-hardware-routing.md
games: [commandw, landmakrj]
addresses: []
status: confirmed
evidence:
  - {kind: doc, ref: raw/docs/otis-esp-dac-routing-2026-10-09.txt, note: "12's board-routing observation; user states its reportings match hardware"}
  - {kind: doc, ref: raw/docs/otis-esp-routing-12-remark-2026-10-10.txt, note: "user relays 12's index-swap remark and explicitly vouches that the reportings match hardware"}
  - {kind: doc, ref: raw/docs/taito-f3.md, note: "submodule pinned at a070cbd; 5505-5510 routing note was added upstream in eaa5a0a and is absent from this checkout"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_en.cpp, note: "MAME ES5505-to-pump routes; comparison only, lower-ranked than hardware"}
  - {kind: test, ref: raw/tests/2026-10-10-otis-esp-routing-experiments.md, note: "runtime reference/LLE instrumentation and muting/gain experiments; not hardware routing evidence"}
  - {kind: test, ref: raw/tests/2026-10-10-enhanced-effects-hardware-routing.md, note: "user-reported Enhanced hardware-routing test and 2400-frame Land Maker measurement"}
contradictions:
  - "12's chips.txt ES5510 pin labels (pins 12–15 = SER3..SER0, from the ES5510 datasheet) and fcm.txt labels differ from channel numbering in 12's routing note; contradiction is flagged for user/12, while the routing conclusion follows the hardware observation. The relevant submodule files are pinned at a070cbd; routing note was added in upstream commit eaa5a0a and is not in this checkout."
  - "Earlier analysis hypothesized reversed register-to-pin numbering so MAME already matched hardware; hardware evidence refutes that conclusion. Earlier analysis claimed 12's pair swap would break the mix; hardware evidence refutes that claim."
supersedes: []
---

# Hardware vs MAME — OTIS/ESP audio routing

**Confirmed hardware routing:** OTIS (ES5505) channel 0 is unconnected; channel 1 feeds ESP (ES5510) channel 3, channel 2 feeds channel 2, and channel 3 feeds channel 1. ESP channel 0 outputs to the DAC. 12's board observations are hardware evidence, and the user explicitly states these reportings match hardware; this outranks MAME, datasheet pin labels, and our ESP-microprogram analysis. ^[raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/docs/otis-esp-routing-12-remark-2026-10-10.txt]

12's routing note states: “5505 has 4 serial output channels: 0 unused, 1→5510 channel 3, 2→5510 channel 2, 3→5510 channel 1. 5510: channel 0 outputs to DAC; channels 1/2/3 input from 5505 channels 3/2/1.” The note is absent from the current submodule checkout (pinned at a070cbd); it was added upstream in eaa5a0a. Do not change the submodule pin. ^[raw/docs/taito-f3.md]

In MAME pump terms, OTIS pair 1 → ESP serial inputs 4/5, pair 2 → 2/3, pair 3 → 0/1, and pair 0 → nothing. MAME's current routing instead sends pairs straight through (pair 1 to 0/1, pair 2 to 2/3, pair 3 to 4/5) and also includes pair 0 as an Aux bypass; thus MAME swaps pairs 1 and 3 relative to hardware and includes an erroneous pair-0 bypass. ^[raw/emu-source/mame-0.289/taito_en.cpp L261-L269; raw/docs/otis-esp-dac-routing-2026-10-09.txt]

12 remarked that this is the second 1↔3 index swap found on F3. The other instance is not recorded in the current wiki pages; the recorded PF1/PF3 vertical-zoom swap is a different kind of swap, not an index-routing instance. ^[raw/docs/otis-esp-routing-12-remark-2026-10-10.txt; raw/docs/taito-f3.md]

## Contradictions and superseded analysis

12's `chips.txt` labels ES5510 pins 12–15 as SER3..SER0 (matching the ES5510 datasheet labels), and `fcm.txt`'s labels also differ from the channel numbering in 12's routing note. This is a source-level contradiction flagged for the user/12; we do not resolve it here. The wiring conclusion follows the hardware observation regardless of pin-label interpretation. The labels are cited at upstream commit eaa5a0a, not available in the a070cbd checkout. ^[raw/docs/taito-f3/chips.txt; raw/docs/taito-f3/fcm.txt; raw/docs/taito-f3.md]

Earlier analysis proposed reversed register↔pin numbering as a way for MAME to already match hardware. This is refuted by the higher-ranked hardware observation. Earlier analysis also claimed 12's swap would break the mix; that claim is likewise refuted by hardware evidence. These earlier inferences remain recorded here as refuted, not erased. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md; raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/docs/otis-esp-routing-12-remark-2026-10-10.txt]

The earlier tentative report had an internally conflicting ESP pin-13 assignment (serio1 and serio0/DAC) and marked OTIS ser0 uncertain. The subsequent channel-routing observation and user's confirmation settle the logical channel routing above; the earlier report remains historical context only. ^[raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/docs/otis-esp-routing-12-remark-2026-10-10.txt]

## Runtime evidence and implications

Runtime instrumentation in Command War and Land Maker found their loaded ESP programs reading SER0–SER2 and writing stereo output only to SER3; pair-muting tests changed output samples in the tested runtime. These are runtime findings, not evidence that overrides the hardware wiring observation. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

MAME and our Reference backend (copied from MAME) do not match hardware routing. Enhanced now implements the hardware wiring: pair 3 (channels 6/7) → ESP inputs 0/1, dry; pair 2 (channels 4/5) → inputs 2/3, delay; pair 1 (channels 2/3) → inputs 4/5, reverb; pair 0 (channels 0/1) is silent. Reference is unchanged and keeps MAME routing. The Enhanced test and 2400-frame Land Maker measurement are recorded in the runtime evidence note. ^[raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/tests/2026-10-10-enhanced-effects-hardware-routing.md]

The runtime gain experiment found changing emulated OTIS→ESP gain from 0.18 to 1.0 raised Command War RMS by 5.55×; this describes the runtime mix only and does not establish physical link gain. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

See [[hardware/sound]] for the board and sound-CPU context, and [[hardware/fcm]] for FCM pin-label context. ^[raw/docs/otis-esp-dac-routing-2026-10-09.txt; raw/docs/taito-f3.md]
