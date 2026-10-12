---
title: Land Maker (Japan) — landmakrj
created: 2026-10-09
updated: 2026-10-12
type: game
tags: [game-loop, video, rom-layout, eeprom, sound-driver]
sources:
  - raw/tests/landmakrj-round-gate-watch.md
  - raw/tests/landmakrj-2p-versus-trigger-runs.md
  - raw/tests/landmakrj-differential-sync-experiment.md
  - raw/docs/landmakrj-config-lanes.toml.txt
  - raw/docs/landmakrj-config-excerpts.toml.txt
  - raw/docs/runtime-decisions-rom-set-identity.md
  - raw/docs/runtime-game-config-reference.md
  - raw/tests/landmakrj-ff-fill-extents.md
  - raw/tests/landmakrj-rom-exclusion-profile.md
  - raw/tests/landmakrj-sound-rom-structure-check.md
  - raw/tests/landmakrj-sound-image-crc.md
  - raw/tests/landmakr-vs-landmakrj-config-diff.md
  - raw/tests/landmakrj-seed5-sound-trace.md
  - raw/emu-source/landmakrj-boot.cpp.txt
  - raw/docs/runtime-eeprom-and-fast-boot.md
  - raw/disasm/landmakrj-sound-driver-excerpts.md
games: [landmakrj]
addresses: [0x00007030, 0x00010000, 0x00020000, 0x00088000, 0x0011B362, 0x001FFFFE, 0x00C1089E, 0x00C1E45A, 0x00C20000, 0x00C39A30, 0x00C80000]
status: hypothesis
evidence: []
contradictions: []
supersedes: []
---

# Land Maker (Japan) — landmakrj

A falling-block puzzle game with 1P versus CPU and 2P versus modes. The ROM identity is in
[[SCHEMA]]. The tool setup is complete:
- `games/landmakrj/config.toml`;
- `games/landmakrj/video/` (writers already known to the renderer);
- `profiles/landmakrj.profile`, which is frozen because it feeds code generation;
- the instrumented build `build-landmakrj-instrument/`.

## Reaching gameplay

The working input scripts are in `build/f3a/landmakrj-inputs/`. They are build output, so copy the
ones a page relies on into `raw/tests/`. ^[raw/tests/landmakrj-round-gate-watch.md]

## ROM layout

Japan 2.01J (`landmakrj`). Everything below is a property of the supplied ROM files or of the
exclusion experiment; none of it is board observation. Code comments in `games/landmakrj/config.toml`
point here. Address ranges are half-open.

**Main program (0x000000..0x200000).** Four 512 KiB lanes interleaved four bytes wide (`e61-13.20` at byte
offset 0, `e61-12.19` at 1, `e61-11.18` at 2, `e61-10.17` at 3); the assembled image hashes to the value in
[[SCHEMA]] (recomputed 2026-10-11). ^[raw/docs/landmakrj-config-lanes.toml.txt] ^[raw/tests/landmakrj-ff-fill-extents.md]

| Interval | What | Evidence |
|---|---|---|
| `0x007030..0x010000` | FF fill | the FF run is `[0x702f, 0x10000)`; the config starts at the first even address ^[raw/tests/landmakrj-ff-fill-extents.md] |
| `0x11b362..0x1ffffe` | FF fill | exact FF run ^[raw/tests/landmakrj-ff-fill-extents.md] |
| `0x1ffffe..0x200000` | checksum word `0x0001`, **retained** (not FF) | ^[raw/tests/landmakrj-ff-fill-extents.md] |
| `0x020000..0x088000` | graphics / script-table data bank (0x68000 = 425,984 bytes) | no rooted instruction starts, entries or control references; the profile saw 0 fetched bytes and 147,494 of 425,984 bytes read ^[raw/tests/landmakrj-rom-exclusion-profile.md] |

- Typed descriptor reads (documented HLE helpers reading tile descriptors off the bus) cover 68,904 bytes of
  the data bank; another 12 typed/read bytes overlap helper code and the scanner blocked, not excluded,
  them. Observed reads do **not** prove the unread bytes are data; the whole-bank decision also rests on ROM
  layout and the absence of rooted references. Rooted discovery still reports 22 main and 4 sound
  unresolved transfers. ^[raw/tests/landmakrj-rom-exclusion-profile.md]
- `[[exclude]]` removes only instruction starts; data reads and extension words stay. The runtime fails
  loudly at an excluded target rather than interpreting it. ^[raw/docs/runtime-game-config-reference.md]
- Evidence limit: profile seeds 1-8 x 20,000 frames recorded 103,856 unique fetched and 213,826 unique read
  bytes in the whole main image (sound: 23,984 fetched, 62,488 read), with zero interpreter fallback. A
  profile says nothing about unplayed states. ^[raw/tests/landmakrj-rom-exclusion-profile.md]
- Counterexample: excluding the high-entropy window `0x002000..0x003000` (entropy 7.689694 bits/byte, 252
  distinct byte values, 79 of 128 sampled aligned starts decodable) broke the attract run after 1.024 s with
  `Excluded main CPU instruction at PC 0x2f84 in [0x2000, 0x3000)`. Entropy alone is not proof of data; the
  range is not excluded. ^[raw/tests/landmakrj-rom-exclusion-profile.md]

**Sound program (sound CPU `0xc00000..0xc80000`, 512 KiB).** Two 128 KiB dumps (`e61-14.32`, `e61-15.33`) are
FF-padded to 256 KiB each and interleaved (stride 2); the padded lanes have CRC32 `18961bbb` and `2c64557a`
and the assembled image `5a7e9117`. ^[raw/docs/landmakrj-config-lanes.toml.txt] ^[raw/tests/landmakrj-ff-fill-extents.md] ^[raw/tests/landmakrj-sound-image-crc.md]
The sound reset vector at image offset 4 is `$c1089e`, where execution begins by clearing registers and zeroing RAM. ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
| Interval | What | Evidence |
|---|---|---|
| `0xc1e45a..0xc20000` | FF fill | exact FF run ^[raw/tests/landmakrj-ff-fill-extents.md] |
| `0xc20000..0xc39a30` | sequence/header data bank (0x19a30 = 105,008 bytes) | 0 fetched bytes, 31,834 of 105,008 read ^[raw/tests/landmakrj-rom-exclusion-profile.md]; stream directory at `0xc20008` and header bank at `0xc3879e` each hold 27 entries ^[raw/tests/landmakrj-sound-rom-structure-check.md] |
| `0xc39a30..0xc80000` | FF fill, including the unpopulated half of the 128 KiB chips | exact FF run ^[raw/tests/landmakrj-ff-fill-extents.md] |

The sequence format is on [[routines/landmakrj-00c14730-sequence-event-decode]]. Sample ROMs
(`e61-04.38`..`e61-06.40`) and tile/sprite pixel ROMs are separate runtime regions, never inputs to code
discovery. ^[raw/tests/landmakrj-rom-exclusion-profile.md]

**Japan versus World.** Only the four program lanes differ between the two configs; the sprite, tile,
sound and sample sections of both files are identical apart from one trailing blank line.
^[raw/tests/landmakr-vs-landmakrj-config-diff.md]

| Lane offset | Japan `landmakrj` (CRC32) | World `landmakr` (CRC32) |
|---|---|---|
| 0 | `e61-13.20` (`0af756a2`) | `e61-19.20` (`f92eccd0`) |
| 1 | `e61-12.19` (`636b3df9`) | `e61-18.19` (`5a26c9e0`) |
| 2 | `e61-11.18` (`279a0ee4`) | `e61-17.18` (`710776a8`) |
| 3 | `e61-10.17` (`daabf2b2`) | `e61-16.17` (`b073cda9`) |

^[raw/docs/landmakrj-config-lanes.toml.txt] ^[raw/docs/landmakrj-config-excerpts.toml.txt]

- MAME defines World `landmakr` with the `e61-19.20`/`18.19`/`17.18`/`16.17` lanes; the ROM directory named
  `landmakr` that the project was supplied contains the Japan files, so the two configs stay separate and
  Japan is never silently treated as World.
  ^[raw/docs/runtime-decisions-rom-set-identity.md]
- The World config is config-only: no entry points, no exclusions, default recursive coverage, no video
  folder. Every address and exclusion on this page is a Japan-program address and must not be assumed for
  World; the exclusion scanner failed on the absent `e61-19.20` in the supplied directory.
  ^[raw/docs/landmakrj-config-excerpts.toml.txt] ^[raw/tests/landmakrj-rom-exclusion-profile.md]

## Boot and EEPROM

- The boot-complete signal is the main CPU's `moveq #1,d0; movec d0,cacr` in the boot task at `$8dde8`,
  after the RAM/VRAM self-test, the EEPROM signature check and two 180-frame `trap #4` waits; the failing
  boot path (`$10104`/`$1014c`) writes CACR 0. A throwaway harness measured `cacr == 0` on every frame
  boundary 1..780 with a blank EEPROM and first `1` at frame 781; with an initialised EEPROM `0` for 1..477
  and first `1` at frame 478, with no regression afterwards (the harness was not retained). The
  difference (303 frames) is the game's blank-EEPROM initialisation pass. ^[raw/emu-source/landmakrj-boot.cpp.txt] ^[raw/docs/runtime-eeprom-and-fast-boot.md]
- The EEPROM is a 93C46 of 64 x 16-bit words (128 bytes, big-endian in the runtime's file). On the first
  boot the game writes the whole chip including checksum `$85ac`; with the nominal four-tick CPU model the
  boot instead stopped at `PUSH TEST SWITCH`, and reference opcode costs initialised the checksum and
  reached the tutorial. ^[raw/docs/runtime-eeprom-and-fast-boot.md] Pins and port: [[hardware/fio]].
- The model charges 28,000 cycles (1.75 ms at 16 MHz) per word write, 16,000 per erase and 128,000 for
  write-all/erase-all; adding busy timing removed a roughly seven-frame lead against the MAME baseline (64
  startup writes, about 112 ms). The same record has the first persistent timing difference where an IRQ
  interrupts an EEPROM busy poll and traces a six-scanline offset to the first-IRQ epoch.
  ^[raw/docs/runtime-eeprom-and-fast-boot.md]
- Fast boot (the runtime runs the boot frames unpresented) and the boot-state cache are host features, not
  game behaviour; they are only noted here so the 478/781 numbers are not confused with them.

## Routine index

- [[routines/landmakrj-0008e20c-round-task]]: round task and its round-end gate (hypothesis)
- [[routines/landmakrj-0008edde-versus-mode-transition]]: 2P versus mode latch, join transition, netplay sync set and RNG reseed (hypothesis)
- [[routines/landmakrj-000041d0-sprite-list-writers]]: sprite-list setup, compilers, deferred draw queue, boot self-test (hypothesis)
- [[routines/landmakrj-000a8e0c-display-object-loop]]: display-object loop and object sprite helpers (hypothesis)
- [[routines/landmakrj-00005cd8-line-ram-producers]]: per-layer producer PCs, effect scenes and the fallback conditions (hypothesis)
- [[routines/landmakrj-000030be-sound-selector-dispatch]]: main-side sound selector table and ring writers (hypothesis)
- [[routines/landmakrj-00c12ecc-sound-command-dispatch]]: sound command packet set (hypothesis)
- [[routines/landmakrj-00c10da0-sound-timer-irq]]: sound 1 ms DUART tick and sequence tick (hypothesis)
- [[routines/landmakrj-00c14730-sequence-event-decode]]: sequence/header/program formats (hypothesis)
- [[routines/landmakrj-00c1748e-voice-allocation]]: voice search, priority exhaustion, legato reuse, OTIS setup and voice silencing (hypothesis)
## Evidence and comparisons

- [[comparisons/runtime-vs-mame-attract-capture]]: 25 attract frames match MAME exactly; what that does and does not show.
- Sound hardware pages: [[hardware/sound-mailbox]], [[hardware/duart-and-volume]], [[hardware/es5505-otis]],
  [[hardware/es5510-esp]], backed by ROM driver excerpts in ^[raw/disasm/landmakrj-sound-driver-excerpts.md].
- Seed-5 sound trace: 6000 frames gave 12,258,121 identical native versus interpreted driver records and
  3,029,425 PCM frames (see [[routines/landmakrj-00c10da0-sound-timer-irq]]). ^[raw/tests/landmakrj-seed5-sound-trace.md]

## Quirks relied on

None recorded yet.

See also [[games/commandw]] and [[index]].
