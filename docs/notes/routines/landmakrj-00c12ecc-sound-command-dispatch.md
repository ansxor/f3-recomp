---
title: Land Maker sound driver command dispatch (c12ecc) and command set
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [audio, dispatch, sound-driver, mailbox]
sources:
  - raw/docs/runtime-audio-mailbox.md
  - raw/docs/runtime-hle-audio-rom-protocol.md
  - raw/docs/runtime-audio-sequences.md
  - raw/tests/landmakrj-sound-rom-structure-check.md
  - raw/tests/landmakrj-seed5-sound-trace.md
  - raw/tests/landmakrj-sound-image-crc.md
games: [landmakrj]
addresses: [0x00C12ECC, 0x00C0B414, 0x00C1323E, 0x00C13260, 0x00C13072, 0x00C141CE]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/runtime-audio-mailbox.md, note: "command table derived from sound-ROM analysis in docs/SOUND-DRIVER.md; the runtime documentation did not re-derive it"}
  - {kind: test, ref: raw/tests/landmakrj-sound-rom-structure-check.md, note: "0xc13260 length table and 0xc1323e handler table read from the ROM image and matching the table below"}
  - {kind: trace, ref: raw/tests/landmakrj-seed5-sound-trace.md, note: "seed-5 trace: 852 packets published, consumed and dispatched; every non-startup ES5505 start has a RAM-linked source command"}
contradictions: []
supersedes: []
---

# Sound driver command dispatch

Sound-CPU address space (`0xc00000` ROM window). The ROM is the Japan sound program, CRC32 `5a7e9117` of the
assembled 0x80000-byte image (recomputed from the lane files). ^[raw/docs/runtime-audio-sequences.md] ^[raw/tests/landmakrj-sound-image-crc.md] Full driver evidence lives in
`docs/SOUND-DRIVER.md` (stays in the repository); this page keeps the command grammar and addresses
needed to read packets. Framing and the ring are on [[hardware/sound-mailbox]]; the main-side producer is
[[routines/landmakrj-000030be-sound-selector-dispatch]].

## Stages of one packet

Submit, consume, dispatch, note allocation and the first voice write happen at different times:

1. The sound IRQ handler polls the ring (`c110c2`) and consumes the packet (length at `c11116`), queueing a
   task message with the ring offset. ^[raw/docs/runtime-audio-mailbox.md]
2. A task later dispatches it: `c12ecc` for the high commands, `c0b414` for the effect commands.
   ^[raw/docs/runtime-audio-mailbox.md]
3. The handler changes sequence/channel/note state; the note factory allocates a note node; voice init
   programs the ES5505 ([[routines/landmakrj-00c14730-sequence-event-decode]]).
   ^[raw/docs/runtime-audio-mailbox.md]

Matching an audio event to the last packet seen is wrong; the decoder follows ring offsets. Invalid
opcode or length can be consumed with no effect. ^[raw/docs/runtime-audio-mailbox.md]
In the seed-5 trace all 2,202 non-startup ES5505 starts had a RAM-linked source command (277 direct,
1,925 sequenced). ^[raw/tests/landmakrj-seed5-sound-trace.md]

## Command set

Handler table `0xc1323e` (17 entries) and accepted-length table `0xc13260` (17 bytes) were read from the
ROM and agree with the documented handlers and lengths; the table entries are 16-bit low halves of
`0xc1xxxx` addresses (width inferred from the matching values). ^[raw/tests/landmakrj-sound-rom-structure-check.md]

| Opcode | Length | Payload | Effect (handler) |
|---|---:|---|---|
| `0x20` | 3 | effect | select effects configuration (< 13), queue effects worker (`$c0b454`) |
| `0x21` | 4 | parameter, value | store byte at `$c84a + 2*parameter` (`$c0b47e`) |
| `0x80` | 3 | sequence | stop and restart, non-looping (`$c12f20`, start `$c12ce0`) |
| `0x81` | 3 | sequence | stop and restart, looping (`$c12f26`, start `$c12ce6`) |
| `0x82` | 3 | sequence | stop and remove (`$c12f1a`) |
| `0x83` | 3 | sequence | rate to zero, release track notes (`$c12f2c`) |
| `0x84` | 3 | sequence | restore saved rate (`$c12f42`) |
| `0x85` | 5 | sequence, pos hi, pos lo | seek (`$c1301a`) |
| `0x86` | 4 | sequence, volume | sequence volume at `+$20`, clear fade step `+$22` (`$c12f62`) |
| `0x87` | 4 | sequence, value | channel `+0x26/+0x2d` of all active tracks (`$c12f96`) |
| `0x88` | 4 | sequence, rate | current and saved rate (`$c12f7a`) |
| `0x89` | 7 | sequence, start, end, rate index, unused | fade with multiplier table (`$c12fcc`) |
| `0x8a` | 5 | sequence, track, value | channel `+0x20/+0x2c` (`$c130c6`) |
| `0x8b` | 5 | sequence, track, value | channel `+0x26/+0x2d` (`$c130ba`) |
| `0x8c` | 6 | sequence, track, index, value | controller routine (`$c130d2`) |
| `0x8d` | 6 | sequence, track, program hi, program lo | select instrument for a track (`$c130da`) |
| `0x8e` | 6 | sequence, track, key, velocity | sustained note, duration `$7fff` (`$c130ea`) |
| `0x8f` | 5 | sequence, track, key | release the first matching note (`$c1313c`, release `$c141ce`) |
| `0x90` | 6 | sequence, track, key, value | pops return PC into A3 (`$c1310e`); matching path attempts a write to ROM `$c1314e` (ignored by the bus), no effect on note pressure (`$c13146`) |

^[raw/docs/runtime-audio-mailbox.md] ^[raw/docs/runtime-hle-audio-rom-protocol.md]

- `0x8f` has no instance ID: it releases the first matching sequence/track/key. `0x8e` enters the same
  note factory as music (at `$c140e4`, A5 = note node, A1 = channel, A6 = track). Track lookup
  `$c13072` reads four argument bytes even for five-byte packets; the fade length-7 packet reads only
  four operands and the last byte is unexplained. ^[raw/docs/runtime-hle-audio-rom-protocol.md] ^[raw/docs/runtime-audio-mailbox.md]
- The field names for `0x87`, `0x8a`, `0x8b` are not inferred from captured runs, and `0x90`'s ignored ROM
  write is established from the driver code, not an audible effect. ^[raw/docs/runtime-audio-mailbox.md]
- Bit 7 of the sequence number is not always ignored: selection lists 7/14/18/24 need the high bit
  (`81 07` is silent; `81 87` selects child 4, then child 5 at the section boundary). Mailbox starts first
  stop/reset the selection. ^[raw/docs/runtime-hle-audio-rom-protocol.md]

Example packets (valid): `038108` (loop sequence 8), `04860874` (volume `0x74`), `068d01074002`
(program `0x4002`, track 7), `068e01072768` (key `0x27`, velocity `0x68`), `058f010727` (release).
^[raw/docs/runtime-audio-mailbox.md]

Related: [[games/landmakrj]], [[hardware/es5510-esp]] (effects commands `0x20`/`0x21`).
