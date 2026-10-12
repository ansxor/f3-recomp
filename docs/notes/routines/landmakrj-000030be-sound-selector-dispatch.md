---
title: Land Maker main-side sound selector dispatch (sub_0030be)
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [audio, dispatch, mailbox, sound-driver]
sources:
  - raw/docs/runtime-audio-mailbox.md
  - raw/tests/landmakrj-main-sound-selector-table-check.md
  - raw/tests/landmakrj-seed5-sound-trace.md
games: [landmakrj]
addresses: [0x000030BE, 0x00002FDE, 0x00003270, 0x00006BDE, 0x00006D5E]
status: hypothesis
evidence:
  - {kind: test, ref: raw/tests/landmakrj-main-sound-selector-table-check.md, note: "selector pointer table at 0x6bde has 0x60 entries ending at 0x6d5e; 16 type-0, 77 type-2 and 3 type-3 descriptors; read from the ROM image, not run"}
  - {kind: doc, ref: raw/docs/runtime-audio-mailbox.md, note: "descriptor type layouts and the helper addresses come from docs/SOUND-DRIVER.md"}
contradictions: []
supersedes: []
---

# Main-side sound selector dispatch

The main game does not send opcodes directly; it calls a dispatcher with a **selector**, which indexes a
descriptor table and expands into mailbox packets. A selector is not an opcode and not a voice number.
^[raw/docs/runtime-audio-mailbox.md]

- Dispatcher `0x30be`; selector pointer table at `0x6bde` (long pointers); the maximum selector `0x5f` is
  stored at `0x6d5e`. The ROM shows the table is exactly `0x60` entries long, ending at `0x6d5e`, with
  descriptors in `0x6d60..0x7005`. ^[raw/docs/runtime-audio-mailbox.md] ^[raw/tests/landmakrj-main-sound-selector-table-check.md]
- Descriptor types: **0** `[type, song, volume]` emits command `0x81` (looping start) then `0x86`
  (volume); **2** `[type, unit, velocity, track, program hi, program lo, key, duration]` emits an
  optional `0x8f` for the old note, then `0x8d` and `0x8e` (the main game schedules the duration); **3**
  counted raw bytes go to the ring through the list routine. ^[raw/docs/runtime-audio-mailbox.md]
  In the ROM, 16 descriptors start with type 0, 77 with type 2 and 3 with type 3; selector 0's descriptor
  is `03 06 03 80 01 03 80 02 02 01` (a type-3 body of two three-byte packets, opcode `0x80`, sequences 1
  and 2, followed by the next descriptor's bytes), which fits the documented layout. This is a static
  reading, not an observed run. ^[raw/tests/landmakrj-main-sound-selector-table-check.md]
- Packet append helpers: `0x2fde` copies one packet and `0x3270` a list into the ring, masking each byte
  index with `0x3ff`, then publishes the doubled producer index ([[hardware/sound-mailbox]]).
  ^[raw/docs/runtime-audio-mailbox.md]
- Master-gain requests are written by `0x3400/0x3404/0x340a/0x3410` ([[hardware/duart-and-volume]]). ^[raw/tests/landmakrj-seed5-sound-trace.md]

Consumer side: [[routines/landmakrj-00c12ecc-sound-command-dispatch]]. Game page: [[games/landmakrj]].
