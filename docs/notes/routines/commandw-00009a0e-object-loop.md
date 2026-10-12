---
title: Command War object draw loop (0x9a0e) and its sprite-RAM writers
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [sprite, object-system, game-loop]
sources:
  - raw/docs/landmakrj-config-excerpts.toml.txt
  - raw/emu-source/commandw-sprite-behaviours.hpp.txt
  - raw/docs/runtime-sprite-units-commandw.md
games: [commandw]
addresses: [0x00009A0E, 0x00009A3C, 0x00009A6C, 0x00009B32, 0x00009D32, 0x0000A1C8, 0x0000A334, 0x0002FED6, 0x0000680A, 0x00408328]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/landmakrj-config-excerpts.toml.txt, note: "config comments: sprite-check attributed every outside sprite-RAM write over 6000 attract frames to the listed frame-writer PCs; the object unit replays bit-exact"}
  - {kind: doc, ref: raw/emu-source/commandw-sprite-behaviours.hpp.txt, note: "disassembly-derived comments in this runtime's behaviour header"}
contradictions: []
supersedes: []
---

# Command War object draw loop

Program addresses of the Command War 0.0J prototype (ROM hash in [[SCHEMA]]). All of it comes from
disassembly comments and `f3rt-tool sprite-check` over 6000 attract frames recorded in this runtime's config
and behaviour header, not from a gameplay-observed trace. ^[raw/docs/landmakrj-config-excerpts.toml.txt]
Static reading shows what the code can do; the sprite-check replay is the only dynamic evidence.
^[raw/docs/runtime-sprite-units-commandw.md]

## The loop

- The object loop at `0x9a0e` sets `a6` to the current object: records of `0x80` bytes starting at
  `0x410000` (`a6 = 0x410000 + k*0x80`). `a5 = 0x410000` is the globals base. ^[raw/docs/landmakrj-config-excerpts.toml.txt]
- Per object: `0xa1c8` projects (the span also replays `0xa3ee`, which recomputes zoom `$4`/`$5`), `0xa334`
  rescales zoom for the level-of-detail class, and `0x9b32` compiles sprite entries through `a4`.
  `0x9a6c` is the loop's `lea $80(a6),a6`, so `[0x9a3c, 0x9a6c)` draws exactly one object.
  ^[raw/docs/landmakrj-config-excerpts.toml.txt] ^[raw/emu-source/commandw-sprite-behaviours.hpp.txt]
- Object record fields used by the runtime: byte 1 bits 2-3 = level-of-detail class `k` (0 = full detail),
  set by the selector at `0x2fed6` from the projected zoom (thresholds `0x80/0xc0/0xe0`); word `$2` =
  descriptor of the level currently drawn (via table `0x5a02c` in `0x9d32`); word `$2a` = descriptor of the
  full-detail level (0 = no mip chain); `$2 = $2a + k*stride`. `0xa334` computes
  `$4 = ($4 - T[k]) << k` with `T = {0, 0x80, 0xc0, 0xe0}` at `0xa372`. Verified on live objects, e.g.
  base `0x0485` has L0..L3 grids 10x12, 5x6, 3x3, 2x2 tiles. ^[raw/emu-source/commandw-sprite-behaviours.hpp.txt]
- The mip level is padded to whole coarse tiles and the selector runs before projection, so a stale class
  can let the real draw clamp at 1:1 while a full-detail replay keeps growing. ^[raw/docs/runtime-sprite-units-commandw.md]

## Shadow flicker

- `0x680a  addq.w #1,-$7cd8(a5)` increments a 16-bit counter once per game-loop iteration: word
  `0x408328`, parity byte `0x408329` (computed: `0x410000 - 0x7cd8`). ^[raw/docs/runtime-sprite-units-commandw.md]
- `0x9b32` compiles one object: if record byte 1 bit 7 is clear (no shadow) it jumps to `0x9d32`; if
  counter parity bit 0 is clear it also jumps to `0x9d32`. Otherwise `0x9b4c..0x9d30` emit the shadow
  entries (descriptor `$40`, offsets `$42-$44`, flip/anchor bits) and `0x9d32` the object body. So the game
  fakes translucent shadows by emitting them only on odd counter parity; because the loop often runs every
  other frame the original shadow is on for two frames, off for two. ^[raw/docs/runtime-sprite-units-commandw.md] ^[raw/emu-source/commandw-sprite-behaviours.hpp.txt]

## Sprite-RAM writers outside the object loop

Every other sprite-RAM write observed in 6000 attract frames comes from these PCs (config comments):
`0x9984..0x9a00` per-frame list head (command words at `0x600324`/`0x600334`, both banks, selected by
`-$8000(a5)`, first frame 585); `0x9a74..0x9af6` tail padding clears after the loop and the final command
word (`0x9aea`/`0x9af0`); `0x0440..0x045c` boot clear of all sprite RAM; `0x0540..0x0592` mode-init bank
clears and initial entry copy from `0x644`; `0x0a9e..0x0aee` initial list head/tail words; `0x4736..0x4742`
generic memclr (seen only clearing a bank at boot, frame 14); `0x514c..0x519a` level/screen init clearing
list words and both banks (first frame 270). ^[raw/docs/landmakrj-config-excerpts.toml.txt]

See [[games/commandw]] and the sprite concepts on [[hardware/sprites]]; the Land Maker counterpart is
[[routines/landmakrj-000a8e0c-display-object-loop]].
