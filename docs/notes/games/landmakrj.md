---
title: Land Maker (Japan) — landmakrj
created: 2026-10-09
updated: 2026-10-09
type: game
tags: [game-loop, video]
sources: [raw/tests/landmakrj-round-gate-watch.md]
games: [landmakrj]
addresses: []
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

## Routine index

- [[routines/landmakrj-0008e20c-round-task]]: round task and its round-end gate (hypothesis)

## Quirks relied on

None recorded yet.

See also [[games/commandw]] and [[index]].
