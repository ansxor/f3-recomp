---
title: Land Maker sound driver sequence streams, events, programs and sample splits (c14730)
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [audio, sound-driver, undocumented]
sources:
  - raw/docs/runtime-audio-sequences.md
  - raw/docs/runtime-hle-audio-rom-protocol.md
  - raw/tests/landmakrj-sound-rom-structure-check.md
games: [landmakrj]
addresses: [0x00C14730, 0x00C1475A, 0x00C148B2, 0x00C20000, 0x00C3879E, 0x00C00134, 0x00C1AE76, 0x00C17CFA, 0x00C1748E]
status: hypothesis
evidence:
  - {kind: test, ref: raw/tests/landmakrj-sound-rom-structure-check.md, note: "stream directory at 0xc20008 and header directory at 0xc3879e+8 each hold 27 entries; header stride 0xa8; read from the ROM image"}
  - {kind: doc, ref: raw/docs/runtime-hle-audio-rom-protocol.md, note: "stream/header/instrument formats from the deleted tools/hle_sequence.py and tools/hle_instruments.py; those tools are no longer in the repository, so the event and instrument counts below were not reproduced"}
contradictions: []
supersedes: []
---

# Sequence streams, events, programs and sample splits

How the Land Maker sound ROM stores music and instruments and how the driver walks them. Command side:
[[routines/landmakrj-00c12ecc-sound-command-dispatch]]; timing: [[routines/landmakrj-00c10da0-sound-timer-irq]].
Hardware the voices end up on: [[hardware/es5505-otis]]. ROM bank placement: [[games/landmakrj]] (ROM layout).

## Banks and directories

- Stream bank at `0xc20000` (long offset at base `+8 + 4*ID`), header bank at `0xc3879e` (word offset at
  base `+8 + 2*ID`). Reading the ROM shows 27 nonzero entries in each directory and header entries 0xa8
  bytes apart. ^[raw/docs/runtime-audio-sequences.md] ^[raw/tests/landmakrj-sound-rom-structure-check.md]
- The documentation reports 27 sequences, 206 tracks and 24,470 events; only the 27 is reproduced
  here. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- Header (`0xa8` bytes): eight 14-byte channel records from `+0x20`, eight auxiliary bytes at `+0x90`,
  effect controls at `+0x98`, a linked-sequence selector at `+0xa6`. A stream block begins with a long
  inclusive size and eleven long track offsets; track lengths are not reliable end bounds (directory
  bounds and the `E9` event are). ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- RAM tables: 100 sequence slots at `0x5e5c` (stride `0x28`), active list at `0x6dfc`, bank bases at
  `0xd404` (stream) and `0xd408` (header), channel pointers at `0x5daa`, voice records at `0xd94a`
  (stride `0xaa`). ^[raw/docs/runtime-audio-sequences.md]

## Event stream

- Routines `0xc14730`/`0xc1475a` scan big-endian words for bit 15 set. The high seven bits of the operand
  are the delay **after** the event. Low byte class: `00..57` note (duration = low 10 bits, a following
  word when zero; packed velocity at `0xc148b2`), `58..af` and `b0..d8` callback classes (controllers
  `B0..D8`), `D9` program/bank select, `DC` relative note reference, `E6` long delay, `E7` arrangement
  entry, `E9` track end. ^[raw/docs/runtime-audio-sequences.md] ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- The note factory adds `0x15` (21) to the decoded key; mailbox `0x8f` applies the same adjustment before
  matching. The same stream can be played, scanned or positioned through a caller-provided callback
  table, so an event is not always a note onset. ^[raw/docs/runtime-audio-sequences.md]
- Repeat marker `100` is the loop-selection point; the callback skips it and returns to the following
  phase at `E9`. Notes belong to the child sequence. ^[raw/docs/runtime-hle-audio-rom-protocol.md]

## Programs, layers and sample splits

- Program table at `0xc00134`; the documentation decodes 100 programs `$4000..$4063` (79 layered, 21
  direct/key-region), packed in alternating bank columns with stride `$198`. A zero enable mask at `+$196`
  selects seventeen 20-byte key-region records; other masks select up to three layers (not reproduced).
  ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- Sample-split records are 14 bytes at `0xc1ae76` (word offsets from `0xc1c1e0`): tuning at `+0`, cutoff key
  at `+5`, loop-mode byte at `+9`, packed address/control longwords at `+2`, `+6`, `+10`. Routine
  `0xc17cfa` picks the first split whose cutoff is at least the effective key. The packed address carries
  flags and OTIS fixed-point bits (Q9 sample-word positions with bank bits), not a byte offset.
  ^[raw/docs/runtime-audio-sequences.md] ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- The metadata seen at sound RAM `0x711a` is ROM metadata copied into RAM, not evidence of a runtime
  override at `0xd840`; the runtime documents the oracle snapshot for the direct `$4002`/key 39/velocity
  104 example. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- Voice allocation (`0xc1748e` then `0xc17670`/`0xc17678`): search matching voices, then the free list
  (`0xd844`), then scan released/active lists by priority class (patch `+0x60` bits 6-7); `0xc176f8` can
  reject with carry set. It is not oldest-voice stealing. ^[raw/docs/runtime-audio-sequences.md]
- Bend range: the byte at `0xc085f2` is `0x02` in this ROM (`0x0c` in Twin Cobra II) and is read
  dynamically; the code bank `0xc08000..0xc1ffff` is byte-identical to the other four tested Taito F3 sets
  except that byte. ^[raw/docs/runtime-hle-audio-rom-protocol.md] ^[raw/tests/landmakrj-sound-rom-structure-check.md]

Not established: the meaning of several voice tag fields and of command `0x87`/`0x8a`/`0x8b` channel
fields; the sources preserve them as raw values. ^[raw/docs/runtime-audio-sequences.md]
