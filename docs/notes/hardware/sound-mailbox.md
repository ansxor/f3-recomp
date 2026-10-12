---
title: Main to sound CPU mailbox (shared dual-port RAM, command ring)
created: 2026-10-11
updated: 2026-10-11
type: hardware
tags: [audio, io, memory-map, mailbox]
sources:
  - raw/docs/runtime-audio-mailbox.md
  - raw/docs/runtime-hle-audio-rom-protocol.md
  - raw/tests/landmakrj-sound-rom-structure-check.md
  - raw/docs/taito-f3/sound-address.txt
  - raw/emu-source/mame-0.289/taito_f3.cpp
games: [landmakrj]
addresses: [0x00C00000, 0x00C003FF, 0x00C00480, 0x00C00482, 0x00C007F8, 0x00140000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/runtime-audio-mailbox.md, note: "runtime documentation; ring layout and packet format were taken from ROM-driver analysis (docs/SOUND-DRIVER.md) and the sound-extraction tools"}
  - {kind: test, ref: raw/tests/landmakrj-sound-rom-structure-check.md, note: "command length table at 0xc13260 read from the ROM matches the documented packet lengths"}
contradictions: []
supersedes: []
---

# Main to sound CPU mailbox

How the main CPU hands commands to the sound CPU through the shared 2 KiB dual-port RAM. The RAM and its
decode are on [[hardware/sound]] (main `0xc00000-0xc007ff`, sound `0x140000`, even bytes); this page is
what the Land Maker software does on top of it. The ring layout comes from ROM driver analysis in this
repository (`docs/SOUND-DRIVER.md`), not from the board. ^[raw/docs/runtime-audio-mailbox.md]
The full driver grammar is also in `docs/SOUND-DRIVER.md`; the command table is in
[[routines/landmakrj-00c12ecc-sound-command-dispatch]].

## Byte lanes and handshake

- The main CPU sees one byte per address at `0xc00000-0xc007ff`; the sound CPU sees byte `i` at
  `0x140000 + 2*i` and reads `0xff` from odd addresses. A 16-bit sound read of a mailbox byte is
  `(byte << 8) | 0xff`; a 16-bit sound write stores the high byte. ^[raw/docs/runtime-audio-mailbox.md]
- There is no lock and no interrupt for the mailbox on either side in the model; the sound driver polls the
  ring from its timer interrupt ([[routines/landmakrj-00c10da0-sound-timer-irq]]).
  ^[raw/docs/runtime-audio-mailbox.md]
- Command ring: 1024 bytes at main `0xc00000-0xc003ff` (sound `0x140000`..). Producer index at main
  `0xc00480-0xc00481` (sound `0x140900`), consumer index at `0xc00482-0xc00483` (sound `0x140904`), both
  big-endian and **doubled** (value = 2 x byte index); the ring wraps with `& 0x3ff`.
  ^[raw/docs/runtime-audio-mailbox.md] ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- Main is the producer: it copies the packet into the ring and then writes the new producer value, the
  low byte at `0xc00481` last, which is the commit. The sound driver consumes (length at `$c11116`,
  opcode at `$c11128`, consumer index advance at `$c11146-$c11150`, published with `MOVEP`). Main routines
  `$2fde` (one packet) and `$3270` (a list) append packets. ^[raw/docs/runtime-audio-mailbox.md]
- An older project note said the ring has 2048 command bytes; the later ROM analysis corrects this to 1024
  bytes inside the 2048-byte RAM. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- The free-space rule that keeps one byte to tell full from empty is the extraction tool's injection
  policy, not a measured hardware limit. ^[raw/docs/runtime-audio-mailbox.md]

## Packet framing

Byte 0 is the total length including itself and the opcode, byte 1 the opcode, the rest big-endian
operands. For the high commands (`0x80+`) the driver clears bit 7 of the opcode, requires index < `$11` and
the exact length from a table. ^[raw/docs/runtime-audio-mailbox.md]
That table is at `0xc13260`: reading the ROM gives `3,3,3,3,3,5,4,4,4,7,5,5,6,6,6,5,6` for opcodes
`0x80..0x90`, matching the documented lengths. ^[raw/tests/landmakrj-sound-rom-structure-check.md]

## Other mailbox bytes

- Main writes four master-gain requests to `0xc007f8-0xc007fb` during boot (observed value `$30`); timing
  is on [[hardware/duart-and-volume]]. ^[raw/docs/runtime-hle-audio-rom-protocol.md]
- The sound CPU can write replies into the same RAM (a unit test checks the ordering); the Land Maker use
  of any reply area is not documented in the sources. ^[raw/docs/runtime-audio-mailbox.md]

Unverified here: the dual-port chip's own behaviour (MB8421 busy/arbitration) is not modelled by any source
used. Hypothesis: decoded by the FCM/PALs ([[hardware/fcm]], [[hardware/pal-decoders]]).
