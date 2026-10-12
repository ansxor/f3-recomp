---
title: Land Maker versus mode transition (sub_08edde)
created: 2026-10-10
updated: 2026-10-11
type: routine
tags: [game-loop, task-system]
sources: [raw/tests/landmakrj-desync-695b-frame540.md, raw/tests/landmakrj-lobby-tail-desync-and-flash.md, raw/tests/landmakrj-2p-versus-trigger-runs.md, raw/disasm/landmakrj-0008edde-versus-mode-transition.md, raw/tests/landmakrj-differential-sync-experiment.md, raw/emu-source/landmakrj-netplay.cpp.txt, raw/tests/landmakrj-rng-reseed-audit.md]
games: [landmakrj]
addresses: [0x0008EDDE, 0x0008EE06, 0x0008F0D6, 0x0008F79A, 0x0009242C, 0x00401F6E, 0x00401F53, 0x00401F7E, 0x00401F5C, 0x00401F6A, 0x00401F6C, 0x00401F70]
status: hypothesis
evidence:
  - {kind: test, ref: raw/tests/landmakrj-2p-versus-trigger-runs.md, note: "0x401f6e transitions 0 -> 1 at frame 2065 in 2P versus, frame 1450 in mid-charsel join, and frame 2300 in mid-game challenger; stays 1 across 9,335 frames of 2P play; clears at frame 11400 (at 0x08e5f2); refires at frame 13766 for a second 2P match"}
  - {kind: doc, ref: raw/disasm/landmakrj-0008edde-versus-mode-transition.md, note: "move.w #1, -$6092(a5) at 0x08edfe (mode select), 0x08ee2c (charsel join), and 0x08f7ee (challenger); clr.w at 0x08e5f2"}
  - {kind: test, ref: raw/tests/landmakrj-differential-sync-experiment.md, note: "RAM-only minimal sync (550 bytes) diverges at step 1380/1516 due to idle-loop RNG phase drift ($19f8); full host snapshot sync (3.47 MB) achieves 100% byte-exact parity across 5,000 frames"}
contradictions: []
supersedes: []
---

# Land Maker versus mode transition (sub_08edde)

The routine `sub_08edde` runs after mode selection in [[routines/landmakrj-0008e20c-round-task]] to commit the game to 2-player versus mode. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]

## Versus latch (0x401f6e)

The word at 0x401f6e (`a5-$6092`) acts as the game's canonical 2-player versus mode latch: ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
- In attract mode and 1P-vs-CPU play, it remains 0 throughout. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
- In 2-player versus matches, it is set to 1 before character selection begins and remains 1 across all rounds until match over. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
- It is cleared back to 0 at 0x08e5f2 upon match end in [[routines/landmakrj-0008e20c-round-task]]. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]

## Transition sites

The value 1 is written to 0x401f6e at three primary entry paths into 2P versus: ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]

1. **Game Select menu completion (`sub_08edde`)**:
   At 0x08edf8 it tests `-$609a(a5)` (0x401f66). If 0x401f66 is non-zero (set when option 2 '2P 対戦' was chosen in menu task `sub_09e962`), it writes `move.w #$1, -$6092(a5)` at 0x08edfe. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
   In a tested 2P run, this transition occurred at frame 2065, exactly before character select task `sub_09cd04` was created. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]

2. **Mid-character-select join (`sub_08ee06`)**:
   While `sub_08ee06` awaits character select completion, it polls join latch `-$6082(a5)` (0x401f7e) at 0x08ee22. If a second player presses start, join handler `sub_08f0d6` sets 0x401f7e to 1; `sub_08ee06` then clears 0x401f7e and writes `move.w #$1, -$6092(a5)` at 0x08ee2c. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
   In a tested run, this transition occurred at frame 1450 upon Player 2 start pulse. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]

3. **Mid-gameplay challenger (`sub_08f79a`)**:
   During active 1P gameplay, `sub_08f398` detects Player 2 start input with available credits and invokes `sub_08f0d6`, setting 0x401f7e to 1 and active player bits in 0x401f53. Routine `sub_08f79a` displays the 'HERE COMES A NEW CHALLENGER' banner, and at 0x08f7ee clears 0x401f7e and writes `move.w #$1, -$6092(a5)` before launching character selection. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
   In a tested run, this transition occurred at frame 2300 as the banner concluded and character select began. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]

## Player active flags (0x401f53)

The byte at 0x401f53 (`a5-$60ad`) stores active player status: ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
- Bit 0: Player 1 active in match
- Bit 1: Player 2 active in match
- Bit 6: Player 1 selecting character
- Bit 7: Player 2 selecting character

When both players are engaged, `(0x401f53 & 3) == 3`. In the attract mode demo `sub_090d48`, `0x401f53` is also temporarily set to 3 by `move.w #$3, -$60ae(a5)` at 0x090d66 for the AI demo match, while 0x401f6e remains 0. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]

## Match resolution and lifecycle

During 2P versus play, match flow is tracked by the following globals: ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
- `0x401f5c` (`-$60a4(a5)`): round winner (1 = P1, 2 = P2). Set on round end by gameplay routines (`sub_097a9c`). ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
- `0x401f6a` / `0x401f6c`: Player 1 and Player 2 round win counters. Incremented in `sub_09242c` based on 0x401f5c. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]
- `0x401f70` (`-$6090(a5)`): match over flag. Set to 0xffff by `sub_09242c` when round wins reach target `(0x40fc58 >> 1) + 1`. ^[raw/disasm/landmakrj-0008edde-versus-mode-transition.md]

In a verified 15,000-frame two-match run: ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
1. Match 1 trigger fires at frame 2065. `0x401f6e` stays 1 continuously through Round 1 (P2 wins at frame 8000/8200) and Round 2 (P2 wins at frame 10950/11150). ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
2. At frame 11150, P2 wins the match: `0x401f70` becomes 0xffff, bit 4 of 0x401f53 is set (`0x13`, P1 defeated). ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
3. At frame 11400, `sub_08e20c` at 0x08e5f2 clears `0x401f6e` from 1 to 0 and clears round counters. Continue countdown runs frames 11400..12300, returning to attract at frame 12300. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
4. Coins are inserted at frame 13000, and a second 2P versus match is initiated. At frame 13766, `0x401f6e` transitions from 0 to 1, cleanly re-firing the trigger for Match 2. ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]

## Minimal sync list and per-frame RNG reseed

An empirical differential experiment evaluated lockstep determinism between peers starting with different attract durations and cycle phases: ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **Idle-loop RNG drift mechanism**: The 68EC020 idle loop at ROM `$19f8` spins on spare cycles waiting for VBlank, calling ROM `$1468` to advance `0x400826`. Without per-frame reseeding, sub-frame cycle deadline phase differences cause `0x400826` to drift over the 1,380 frames of character select, making `0x401f54` (stage theme) diverge at step 1380 and piece generation diverge at step 1516. ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **Spec 5 / Spec 6C (Minimal 78-byte sync + per-frame reseed)**: Overwriting `0x400826` before each frame via `splitmix32(shared_seed ^ match_frame)` erases idle-loop drift before cooperative tasks run (the shipped hook later changed the key, see "Shipped hook" below). Combined with syncing 78 bytes at the trigger, gameplay and piece generation achieve **100% byte-exact parity across all 8,000 frames of a full match**. ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **96-byte Netplay Sync List (Spec 7, verified across full match)**:
  1. `0x400090..0x40009f` (16 bytes): Credits and coin chute subsystem (credits counter `0x400090`, partial coins `0x400092`/`0x40009e`, debounce states `0x400096`/`0x40009a`). Overwritten on connect with Host's values so continue/challenger permissions and on-screen "CREDIT N" text stay identical.
  2. `0x401f40..0x401f83` (68 bytes): Mode/match block (`0x401f40` game state, `0x401f53` players active/sel, `0x401f54` stage word, `0x401f5c` round winner, `0x401f6a`/`0x401f6c` round wins, `0x401f6e` versus latch, `0x401f70` match over, `0x401f7e` join latch).
  3. `0x40203c..0x40203f` (4 bytes): Player 1 piece generator RNG seed (`+$98(a0)`).
  4. `0x4020e4..0x4020e7` (4 bytes): Player 2 piece generator RNG seed (`+$98(a1)`).
  5. `0x40210e..0x40210f` (2 bytes): In-game play task latch.
  6. `0x40fc58..0x40fc59` (2 bytes): Match rounds setting word (`(0x40fc58 >> 1) + 1` = target wins).
- **Desync detection hash**: Because sound CPU registers, audio clocks, and unused graphics/palette RAM retain attract residues, whole-machine `Machine::state_crc()` differs between peers. Netplay desync detection must hash the gameplay-critical block `0x401f40..0x402130` (496 bytes). ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **Reference oracle**: Test A (full host snapshot transfer via `Machine::save_state` / `load_state`, 3.47 MB) was verified as the reference oracle, confirming that full state restoration also achieves 100% byte-exact parity. ^[raw/tests/landmakrj-differential-sync-experiment.md]

## Shipped hook (sync version 4) and the round-keyed reseed

Snapshot of `games/landmakrj/netplay/netplay.cpp` taken 2026-10-11; the experiments above are the history
that led to it. ^[raw/emu-source/landmakrj-netplay.cpp.txt]
- **Sync blob**: version byte `0x04` plus 527 payload bytes = 528 bytes: credits `0x400090` (16 B), operator
  settings `0x4000cb` (13 B: coinage, free play, difficulty/2P enable, cabinet, coin mode, continue, demo sound,
  free-play config, options), the whole gameplay block `0x401f40..0x40212f` (496 B) and the rounds word
  `0x40fc58` (2 B). Screen flip `0x40013e` is deliberately not synced (presentation only). The earlier 78/96/109 B
  sets were too small because player-structure residue differed per peer (see the frame 540 desync below).
  ^[raw/emu-source/landmakrj-netplay.cpp.txt] ^[raw/tests/landmakrj-desync-695b-frame540.md]
- **Reseed**: `on_match_frame` writes `0x400826` before every lockstepped frame from
  `key = seed ^ (round_index * 0x100000001b3)`, `round_index = word(0x401f6a) + word(0x401f6c)`, value
  `splitmix32(key)`. It depends only on the shared seed and synced RAM, so it survives a snapshot restore, and the
  word is constant within a round. ^[raw/emu-source/landmakrj-netplay.cpp.txt]
- **Audit of `0x400826` readers** (xref plus a 2P 18,000-frame watch run; secondhand record): `sub_001468` (the LCG;
  the idle loop `$19f8` via `sub_001174` advances it, the drift source), `sub_093462` at `0x0935d4` (copies the word into
  both players' bag seeds, once per round about 12 frames after the stage pick), `round_task` at `0x08e470` (stage
  step `((rng & 3) + 1 + prev) & 7` into `0x401f54`), `sub_08e84c` (sound pick), `sub_0915d2` (tile/palette),
  `sub_0a04d0` (dice roll `rng & 0x7f + table[0x4000d0][0x401f54] + 0x401f7a`, script opcode 2, role unconfirmed) and
  a roughly 40-entry `dc.l $1468` callback table (unresolved). `sub_093a1c` swaps the word with the player seed and
  restores it. The wins sum is 0/1/2 at the stage picks at frames 2631/4983/11043 and is updated about 2 frames
  before each pick. ^[raw/tests/landmakrj-rng-reseed-audit.md]
- **Side effect / open**: the `sub_0a04d0` roll and cosmetic readers repeat per round; a round ending without a
  winner would repeat the previous key. Not observed: a round end inside the 16,000-frame two-peer check.
  ^[raw/tests/landmakrj-rng-reseed-audit.md]
- **Desync hash**: CRC32 over 522 bytes, ungated from the first report: the 496 B block minus five cosmetic/residue
  bytes (`0x401f85`, `0x401fa1`, `0x402033`, `0x4020db`, `0x4020f7`; 491 B) plus credits (16), settings (13) and the
  rounds word (2). ^[raw/emu-source/landmakrj-netplay.cpp.txt]

## Lobby hold and un-commit hooks

When the frontend opens a netplay lobby upon 2P versus trigger detection, the game scene holds visually without game logic progression: ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **Pre-commit trigger snapshot point**: Evaluated at frame boundaries on the Game Select screen: `m.read8(0x407902) == 1 && m.read16(0x401f66) == 0xffff` (Option 2 '2P VS' confirmed by B1 press or timer). The frontend captures its local pre-commit snapshot at this frame. ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **Lobby hold (`lobby_hold_frame(Machine &m)`)**:
  Pins RAM words after every `run_frame()` to freeze timers while keeping background swirl (`sub_09ecb0`), sparkles (Task 7 `sub_003d5e`), and BGM track $4e animating: ^[raw/tests/landmakrj-differential-sync-experiment.md]
  - `m.write8(0x407902, 1)` (keeps confirmation state active)
  - `m.write8(0x407903, 30)` (freezes flash counter at 30, preventing the 64-frame exit condition)
  - `m.write16(0x407908, 0x04b0)` (freezes countdown timer at full value 1,200 frames / 10s)
  - `m.write16(0x401f6e, 0)` (prevents premature versus latch trigger until lobby resolution)
- **Cancel netplay un-commit (`uncommit(Machine &m)`)**:
  Returns Game Select to the interactive selecting state when netplay is cancelled: ^[raw/tests/landmakrj-differential-sync-experiment.md]
  - `m.write8(0x407902, 0)` & `m.write8(0x407903, 0)` (clear confirm flag & flash counter)
  - `m.write16(0x407908, 0x04b0)` (reset countdown timer to full 10-second value)
  - `m.write16(0x401f66, 0)` & `m.write16(0x401f6e, 0)` (clear VS mode flags)
  - Clear player input words so no held button re-confirms.
  - Result: Player returns to Game Select menu with cursor usable, timer at full value, and no scene switch or trigger refire. ^[raw/tests/landmakrj-differential-sync-experiment.md]
- **Play locally**: Restore the pre-commit snapshot, unpin, and let the 64-frame confirm animation proceed naturally into Character Select. ^[raw/tests/landmakrj-differential-sync-experiment.md]
## Post-sync frames desync, and the flash counter pin

- **Post-sync lobby frames (desync cause, hypothesis)**: the frontend keeps `in_lobby` true after `capture_sync`/`apply_sync` until the session is Running, so each peer runs a different number of pinned lobby frames on the already-synced machine; with unequal counts the peers desync (late, e.g. completed frame 1346-1349, or at once through the `0x401f6e=0` pin). Equal tails or no tail are fine. Fix: do not advance the machine between sync and match start. ^[raw/tests/landmakrj-lobby-tail-desync-and-flash.md]
- **Flash counter 0x407903**: `sub_09ecf6` draws a white flash overlay sprite while counter <= $20 and `(counter & 4) == 0`, increments each frame, and sets the exit flag when it passes $40. Pin it at `0x30` (any value $21..$3f): calm gold glow, never exits. Pinning the captured small value (usually 1) shows the overlay every frame (solid white). ^[raw/tests/landmakrj-lobby-tail-desync-and-flash.md]

## Unsynced player-struct residue (frame 540 desync)

- The 109 B sync leaves the player-structure residue in 0x401f40..0x40212f different per peer (19 differing bytes at match frame 1 in session 695b9e18). Phase 1 does not hash it; at the gate flip (`0x40210e` 0 -> 1, first instruction of `sub_093462`) the 491 B hash includes bytes that init has not overwritten yet: `0x40206c.w` (writer `sub_093be4`), `0x402078` (`sub_093b06`/`sub_09f9c4`), the 32-bit counter `0x4020f4` (cleared three frames later by `sub_093462`). Fix: sync the whole 496 B block `{0x401f40, 496}`; verified on both bundles and a 6000-frame matrix. ^[raw/tests/landmakrj-desync-695b-frame540.md]
- Bundles come from the frontend machine (GPU game video + enhanced audio, state 1701710 B); `f3rt-tool netplay-replay` builds a reference-audio, no-GPU machine (3468025 B) and refuses them. ^[raw/tests/landmakrj-desync-695b-frame540.md]

## Netplay hook recommendation

The recommended netplay trigger evaluates at frame boundaries: ^[raw/tests/landmakrj-2p-versus-trigger-runs.md]
```cpp
// Fires once on frame boundary when entering 2P versus mode
// (fresh start, mid-charsel join, or mid-game challenger).
// Frontend checks RAM word 0x401f6e (a5-$6092, big-endian uint16_t) every frame boundary.
bool trigger(uint16_t prev_word, uint16_t now_word) {
    return prev_word == 0 && now_word == 1;
}
```

Game page: [[games/landmakrj]].
