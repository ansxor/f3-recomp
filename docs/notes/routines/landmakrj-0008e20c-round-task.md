---
title: Land Maker round task (sub_08e20c)
created: 2026-10-09
updated: 2026-10-09
type: routine
tags: [task-system, game-loop, scoring]
sources: [raw/tests/landmakrj-round-gate-watch.md, raw/disasm/landmakrj-0008e600-round-gate.md]
games: [landmakrj]
addresses: [0x0008E20C, 0x0008E62E, 0x0008E8B8, 0x0008E9C6, 0x00401F54, 0x00401F62]
status: hypothesis
evidence:
  - {kind: test, ref: raw/tests/landmakrj-round-gate-watch.md, note: "branch 0x08e62e never ran in 9000 frames of 2P versus; with 0x401f54 poked to 10 it ran at frame 7286 and sub_08e9c6 at 7287"}
  - {kind: doc, ref: raw/disasm/landmakrj-0008e600-round-gate.md, note: "cmpi.w #$a / #$3 against a5-$60ac before bsr sub_08e8b8"}
contradictions: []
supersedes: []
---

# Land Maker round task (sub_08e20c)

A seeded task (f3a kinds `ref,seed`). Its entry ran once, at frame 970, in a 9000-frame 2P versus
run. Later work resumes inside the routine, so a watch on its entry says nothing about rounds.
^[raw/tests/landmakrj-round-gate-watch.md]

## Round-end gate

At 0x08e616 it tests the word at 0x401f62 (`a5-$609e`):

- If the word is zero, the round counter at 0x401f54 (`a5-$60ac`) must be at least 10.
- Otherwise the counter must be at least 3.

When the counter passes, it calls `sub_08e8b8` at 0x08e62e and continues at 0x08e6d2.
^[raw/disasm/landmakrj-0008e600-round-gate.md]

In 9000 frames of 2P versus with random input (`2p-versus-rounds.txt`), 0x08e62e never ran. With
`3000-end poke 0x401f54.w=10`, it ran at frame 7286, and `sub_08e9c6` (a text writer reached through
`sub_08e8b8` → `sub_08e9c0`) ran at frame 7287. ^[raw/tests/landmakrj-round-gate-watch.md] The
poke shows the path is reachable; it does not show which play situation reaches it naturally.

Hypothesis: 0x401f54 counts round wins in 2P versus, and the gate opens on a 10-win (or 3-win)
match end, the "match over" screen. In the earlier helper runs the counter reached 7 at most, then
the game ended. Those runs are not in `raw/`. Confirm by letting the counter reach 10 without a
poke, or by reading the screen that `sub_08e9c6` draws (`--keep rendered.png` around frame 7287).

Related: [[games/landmakrj]] (input scripts, routine index), [[SCHEMA]] (evidence kinds), [[index]].
