---
title: Runtime versus MAME attract-mode captures (landmakrj)
created: 2026-10-11
updated: 2026-10-11
type: comparison
tags: [video, timing, emulator-bug, eeprom]
sources:
  - raw/tests/landmakrj-mame-attract-capture-parity.md
  - raw/tests/landmakrj-rom-exclusion-profile.md
  - raw/docs/runtime-eeprom-and-fast-boot.md
games: [landmakrj]
addresses: []
status: hypothesis
evidence:
  - {kind: test, ref: raw/tests/landmakrj-mame-attract-capture-parity.md, note: "secondhand record of the 25-frame parity run; command output and captures were not retained"}
contradictions: []
supersedes: []
---

# Runtime versus MAME attract-mode captures

What the repository's MAME comparison does and does not establish. MAME here is a reference emulator, not
the board: a match shows this runtime reproduces MAME's output, and says nothing for or against anything
12 observed on a real PCB. Where MAME differs from a board observation, MAME is the one that is wrong; see
[[comparisons/hardware-vs-mame-audio-routing]] for the confirmed case. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]

## Reference

- MAME source revision `cfc4760a3be9c5a79846b19b6a573cb38459fa7e` (clean tree), binary SHA-256
  `156ffb34020580bc05d9ae0854b72e0f7f7168b1b1f466127d5d0c2f3610b91e`. Expected warnings: the undumped
  `palce16v8q-d77-15.ic21` and a missing-parent diagnostic for `spcinvdj`; neither prevented the captures.
  ^[raw/tests/landmakrj-mame-attract-capture-parity.md]
- The two sound ROM dumps are 128 KiB, MAME expects 256 KiB; appending `0xFF` makes the CRCs match
  (`18961bbb`, `2c64557a`). See [[games/landmakrj]] ROM layout. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]

## Capture protocol (format 2)

- A capture folder per frame holds control registers (rebuilt from a write tap, since they are write-only),
  palette RAM, graphics RAM, main RAM, shared RAM, the sprite RAM MAME actually drew, the reference pixels
  and CPU state. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]
- MAME exposes `screen:pixels()` for the previous completed frame at the frame-done callback, so state is
  taken at callback N and paired with the pixels read at callback N+1; version-1 captures paired current RAM
  with the previous picture and must not be used. An extra 34 consecutive animated frames established this
  one-callback pixel delay. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]
- Land Maker's MAME renderer buffers sprites for one frame: palette, playfield, text and line RAM of frame N
  combine with the sprite list of frame N-1. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]
  Compare [[hardware/sprites]] and [[hardware/video-timing]].

## Results

| Check | Result |
|---|---|
| Attract captures, frames 600-3480 every 120 (25 frames), 320 x 232 | zero RGB mismatches over 1,856,000 pixels (25 x 74,240), maximum channel error 0 |
| Frame-600 main RAM (131,072 bytes) | byte-identical to MAME |
| Cold boot, native strict path, 3,600 frames | 52,342,915 native blocks, zero fallback instructions (a later build reports 51,507,335 blocks, same frames) |
| Earlier, before EEPROM busy timing | MAME frame 1080 equalled native frame 1073 RGB-exactly: a roughly seven-frame lead accounted for by 64 startup EEPROM writes at 1.75 ms |

^[raw/tests/landmakrj-mame-attract-capture-parity.md] ^[raw/tests/landmakrj-rom-exclusion-profile.md]
^[raw/docs/runtime-eeprom-and-fast-boot.md]

The same 25-frame, 1,856,000-pixel check was re-run as a gate for each applied ROM-exclusion set (see
[[games/landmakrj]] ROM layout). ^[raw/tests/landmakrj-rom-exclusion-profile.md]

## Limits

- This validates comparison against captured MAME output, not native game correctness against hardware nor
  physical chip behaviour. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]
- Only attract mode on one ROM set was compared. Audio is not parity: in the one native run recorded in
  `raw/` (seconds 20-54 against the reset-aware MAME baseline) the lag was -4 samples, left/right
  correlation 0.832/0.829 and RMS error 116.7/116.5 PCM LSB. A separate device-replay comparison at the
  native OTIS rate reached correlation 0.999978 and about 1.3 LSB RMS, which isolates the device models
  and not the game. ^[raw/tests/landmakrj-mame-attract-capture-parity.md]
- Everything in the raw record is secondhand: the captures and command output lived under ignored `build/`
  and are not stored in `raw/`, so the numbers cannot be re-derived from the wiki. Re-run
  `tools/mame/run_capture.sh` with the same MAME binary to reproduce.
