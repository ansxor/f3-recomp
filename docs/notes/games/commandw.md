---
title: Command War (prototype) — commandw
created: 2026-10-09
updated: 2026-10-11
type: game
tags: [game-loop]
sources: []
games: [commandw]
addresses: []
status: hypothesis
evidence: []
contradictions: []
supersedes: []
---

# Command War (prototype) — commandw

The ROM identity is in [[SCHEMA]]. There is a config (`games/commandw/config.toml`) and a build
(`build-commandw/`), but no `games/commandw/video/` writer lists yet. An execution profile can be
made with `f3a profile --game commandw --build`.

## Routine index

- [[routines/commandw-00009a0e-object-loop]]: object draw loop, level-of-detail record fields, shadow flicker gate and the sprite-RAM writers outside the loop (hypothesis)

Other findings from agent rounds 1–4 (the interpreter opcode table at 0x40aa) still need their evidence ingested before they get pages.

See also [[games/landmakrj]] and [[index]].
