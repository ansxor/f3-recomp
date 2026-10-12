---
title: Land Maker sprite-list writers (0x41d0..0x4a36), deferred draw queue and boot self-test
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [sprite, game-loop, object-system]
sources:
  - raw/docs/landmakrj-config-excerpts.toml.txt
  - raw/docs/landmakrj-config-entry-points.toml.txt
  - raw/emu-source/landmakrj-video.cpp.txt
  - raw/tests/landmakrj-sprite-census.md
games: [landmakrj]
addresses: [0x000041D0, 0x00004330, 0x000043B0, 0x000043E0, 0x00004422, 0x00004542, 0x00004688, 0x000046C0, 0x0000480C, 0x00010404]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/landmakrj-config-excerpts.toml.txt, note: "config comments: f3rt-tool sprite-check over 6000 frames (attract plus seeded inputs) attributes every sprite-RAM write outside the emit units to these PCs"}
  - {kind: doc, ref: raw/emu-source/landmakrj-video.cpp.txt, note: "known store-PC ranges per compiler, which suppress --discovery-log lines"}
contradictions: []
supersedes: []
---

# Land Maker sprite-list writers

Japan 2.01J. Where sprite RAM (`0x600000..0x60ffff`) gets written; see [[hardware/sprites]] for the entry format
and [[hardware/sprite-command-word]] for the command words. Evidence is the config's recorded
`sprite-check` attribution plus the video code's known-writer list; both are runtime tooling output, not
board observations. ^[raw/docs/landmakrj-config-excerpts.toml.txt]

## Per-frame list setup (outside any emit unit)

- `0x41d0..0x447e`: command entries `0x4330..0x4374` clear both sprite banks word by word; tail/jump
  words `0x42d0..0x4306`, `0x43ba..0x43d4`, `0x43ea`/`0x43f0`, `0x444c..0x447a`. In the config the
  frame-writer ranges are `0x41d0-0x4380`, `0x43b0-0x43de`, `0x43e0-0x43fe`, `0x4400-0x4421`,
  `0x4422-0x447e`. ^[raw/docs/landmakrj-config-excerpts.toml.txt]
- The video code's names for the same PCs: `0x41d0..0x4380` init/clear, `0x43b0..0x43de` sprite scroll,
  `0x43e0..0x43fe` sprite command, `0x4422..0x447e` list terminator. ^[raw/emu-source/landmakrj-video.cpp.txt]
- `0x10404..0x10432` boot RAM self-test: `move.w d1,(a0)` at `0x10412` walks the whole graphics space once
  during boot (about frame 92). ^[raw/docs/landmakrj-config-excerpts.toml.txt]

## Compilers and the deferred draw queue

- `0x4688..0x46be` single-sprite compiler, `0x46c0..0x480a` grid compiler, `0x480c..0x4a36` scaled grid
  compiler. `0x4688` single tiles keep 8-bit X/Y zoom; the scaled grid and `0xa913c..0xa93a2` mask the low
  four X-zoom bits for widths. ^[raw/emu-source/landmakrj-video.cpp.txt] ^[raw/tests/landmakrj-sprite-census.md]
- Deferred draw queue (emit unit `queue`, 18-byte records, unit register `a4`): each start site is a
  `pea <end>(pc)` before a handler reads its record via `(a4)+`; start PCs
  `0x4542, 0x456a, 0x4592, 0x45ba, 0x45f0, 0x4618, 0x4640, 0x4668` with ends `+0x10`
  (`0x4552, 0x457a, 0x45a2, 0x45ca, 0x4600, 0x4628, 0x4650, 0x4678`). The roughly 136 inline push sites
  wrote the records, so ownership is the writer PC of a record's first word. ^[raw/docs/landmakrj-config-excerpts.toml.txt]
- The entry seeds `0x41d0`, `0x43b0`, `0x43e0`, `0x4480`, `0x4528`, `0x4688` and `0x46c0` are in the proven
  seed list. ^[raw/docs/landmakrj-config-entry-points.toml.txt]

Object sprites from the display-object loop: [[routines/landmakrj-000a8e0c-display-object-loop]]. Other
layers' producers: [[routines/landmakrj-00005cd8-line-ram-producers]]. Game page: [[games/landmakrj]].
