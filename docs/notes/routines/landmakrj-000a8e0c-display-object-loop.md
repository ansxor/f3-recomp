---
title: Land Maker display-object loop (0xa8e0c) and object sprite helpers
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [sprite, object-system, game-loop]
sources:
  - raw/docs/landmakrj-config-excerpts.toml.txt
  - raw/emu-source/landmakrj-video.cpp.txt
  - raw/tests/landmakrj-sprite-census.md
  - raw/docs/landmakrj-config-entry-points.toml.txt
games: [landmakrj]
addresses: [0x000A8E0C, 0x000A8E22, 0x000A8E40, 0x000A8F38, 0x000A8F84, 0x000A9036, 0x000A90F4, 0x000A913C, 0x000A93A2]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/landmakrj-config-excerpts.toml.txt, note: "config comments and emit-unit span; object size is recorded there as not yet known"}
  - {kind: doc, ref: raw/emu-source/landmakrj-video.cpp.txt, note: "known sprite store-PC ranges per object helper, used to suppress discovery-log lines"}
contradictions: []
supersedes: []
---

# Land Maker display-object loop

Japan 2.01J addresses. This describes where sprite entries for game objects are written; it is static
reading plus the store-PC lists the video code treats as documented producers, not an observed trace.
^[raw/emu-source/landmakrj-video.cpp.txt]

- Display-object loop at `0xa8e0c`: `a3` = display object; `jmp ([$8,a3])` enters the object's draw handler,
  which returns through `a4 = 0xa8e40`. The object size is not yet known. The emit unit
  `display_objects` is `[0xa8e22, 0xa8e40)` with the unit address in `a3`, owned by the unit.
  ^[raw/docs/landmakrj-config-excerpts.toml.txt]
- Sprite-RAM store PCs of the object helpers (all inside `sprites_covered_write`):
  `0xa8f38..0xa8f82` `obj_grid`, `0xa8f84..0xa8fce` `obj_3tile`, `0xa9036..0xa9076` `obj_grid` with X flip,
  `0xa90f4..0xa913a` `obj_4tile`, `0xa913c..0xa93a2` `obj_scaled`. ^[raw/emu-source/landmakrj-video.cpp.txt]
- `0xa8f38`, `0xa8f84`, `0xa90f4` and `0xa913c` are configured proven entry seeds.
  ^[raw/docs/landmakrj-config-entry-points.toml.txt]
- `0xa913c..0xa93a2` (like `0x480c..0x4a36`) masks the low four X-zoom bits for grid widths while placement
  uses all bits; across a 172,000-frame census there were 259,072 zoomed records out of 32,571,398 and no
  fractional origins. ^[raw/tests/landmakrj-sprite-census.md]

Companions: [[routines/landmakrj-000041d0-sprite-list-writers]] (list setup, deferred queue and the
single/grid/scaled compilers), [[games/landmakrj]], [[hardware/sprites]], and for Command War
[[routines/commandw-00009a0e-object-loop]].
