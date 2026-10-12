---
title: Land Maker sound driver timer interrupt and sequence tick (c10da0, c12e3e)
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [audio, interrupt, timing, sound-driver]
sources:
  - raw/docs/runtime-audio-sequences.md
  - raw/docs/runtime-audio-duart-and-gain.md
  - raw/docs/runtime-hle-audio-rom-protocol.md
  - raw/tests/landmakrj-seed5-sound-trace.md
games: [landmakrj]
addresses: [0x00C10DA0, 0x00C12E3E, 0x00C110C2]
status: hypothesis
evidence:
  - {kind: trace, ref: raw/tests/landmakrj-seed5-sound-trace.md, note: "actual DUART IRQ reads in the seeded trace have modal spacing 16,000 main ticks; counter 0x07d0, ACR 0x60; accumulator threshold 625 at c12e3e"}
contradictions: []
supersedes: []
---

# Sound driver timer interrupt and sequence tick

All driver timing derives from the DUART counter interrupt ([[hardware/duart-and-volume]]); the sample
clock does not advance the sequence. ^[raw/docs/runtime-audio-sequences.md]

- Init: counter preset `0x07d0`, `ACR = 0x60`, `IMR = 0x2b` (observed), giving a 1 ms interrupt
  (16,000 main ticks). The trace's modal IRQ-read spacing is 16,000 main ticks, with jitter from
  instruction boundaries. ^[raw/docs/runtime-audio-sequences.md] ^[raw/tests/landmakrj-seed5-sound-trace.md]
- The IRQ handler at `0xc10da0` reads `0x28001f` (clearing the counter-ready bit), updates task delays,
  ticks the sequencer and polls the mailbox ring (`0xc110c2`). ^[raw/docs/runtime-audio-sequences.md]
  The ring is described on [[hardware/sound-mailbox]].
- Sequence tick: for each active sequence `0xc12e3e` adds rate (`+0`) to an accumulator (`+2`); at the
  threshold `0x271` (625) it subtracts the threshold, advances the subdivision and posts a worker message
  ([[routines/landmakrj-00c12ecc-sound-command-dispatch]]). At a 1 kHz interrupt, rate 120 therefore
  produces 192 ticks per second, 96 per quarter note at 120 BPM (driver arithmetic, not a bus-cycle
  measurement). ^[raw/docs/runtime-audio-sequences.md] ^[raw/tests/landmakrj-seed5-sound-trace.md]
- Pause sets the current rate to zero; resume restores the saved rate at sequence `+0x1c`.
  ^[raw/docs/runtime-audio-sequences.md]
- Software envelopes: a ring at `0xd85a` schedules recurring voice work through `0xc18360` and `0xc18a2e`;
  FC (pitch) writes continue after key-on (e.g. `0xc18296`), so a note-on-only list cannot reproduce the
  audio. ^[raw/docs/runtime-audio-sequences.md]
- HLE-side reading (hypothesis): steady-state physical-voice service once per 12 ms and fades on an
  independent 12 ms signed 8.8 update; header bar boundaries, not the last track's end event, drive
  looping. ^[raw/docs/runtime-hle-audio-rom-protocol.md]

Related: [[games/landmakrj]], [[hardware/es5505-otis]].
