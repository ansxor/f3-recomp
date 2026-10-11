---
source_url: "tools/f3a run (6 scenarios: attract-only, 1p-vs-cpu, 2p-versus, p2-challenger, p2-charsel-join, two-matches-full)"
ingested: 2026-10-10
sha256: af27f19451b53d7d3e5a6361c46a5d816a7cd779dc4b7a3e54b674c2de23381c
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "tools/f3a (git 849cfef), build-landmakrj-instrument"
---
# Land Maker (landmakrj) 2P Versus netplay trigger runs

Runs evaluating candidate trigger predicates across scenarios:
1. Attract only (no player input)
2. 1P vs CPU (P1 coins, starts, picks Normal Mode, plays match)
3. 2P versus (P1 start, selects '2P 対戦' on Game Select menu, confirms)
4. Challenger mid-game (P1 plays 1P match, P2 inserts coin and joins mid-game during round 1)
5. Mid-character-select join (P1 starts 1P match, P2 joins during Character Select)
6. Full match lifecycle, round resolution, latch clearing, and second match refire

## Target RAM variables

- `0x401f6e` (`a5-$6092`, word): Versus latch. 0 in attract and 1P mode; 1 in 2P versus mode. Cleared to 0 at match end (PC 0x08e5f2).
- `0x401f53` (`a5-$60ad`, byte): Active player bits (bit 0 = P1 active, bit 1 = P2 active), charsel flags (bit 6 = P1, bit 7 = P2), and defeat flags (bit 4 = P1 defeated, bit 5 = P2 defeated).
- `0x401f7e` (`a5-$6082`, word): Join request latch. Set to 1 when a second player presses start during character select or mid-gameplay; cleared when handled.
- `0x401f5c` (`a5-$60a4`, word): Round winner (1 = P1 won round, 2 = P2 won round, 0 = in progress / cleared).
- `0x401f6a` (`a5-$6096`, word): Player 1 round wins.
- `0x401f6c` (`a5-$6094`, word): Player 2 round wins.
- `0x401f70` (`a5-$6090`, word): Match over flag (set to 0xffff by sub_09242c when round wins reach target).
- `0x401f66` (`a5-$609a`, word): Game Select menu VS option flag. Set to 0xffff when option 2 ('2P 対戦') is highlighted.
- `0x401f40` (`a5-$60c0`, byte): Game state flags. Bit 0 is 1 in active gameplay, 0 in attract mode.

---

## Scenario A: Attract only

Script: `build/f3a/landmakrj-inputs/attract-only.txt`
```text
# Attract mode only: no coins or inputs
```

Command:
`uv run f3 analyze run --game landmakrj --frames 1500 --inputs build/f3a/landmakrj-inputs/attract-only.txt --dump-every 100 --out build/f3a/run-attract`

Observed:
- `0x401f6e` (word): value 0 across all frames (min 0, max 0).
- `0x401f53` (byte): value 0 across all frames (during title/attract).
- Trigger `prev_word == 0 && now_word == 1`: never fires (0 times).

---

## Scenario B: 1P vs CPU

Script: `build/f3a/landmakrj-inputs/1p-vs-cpu.txt`
```text
# 1P vs CPU: P1 inserts coin, starts, selects Normal mode (option 1), plays
700+20 coin
800-1100 mash keys=start period=45
1350+10 b1
1600-end mash keys=b1 period=45 seed=3
```

Command:
`uv run f3 analyze run --game landmakrj --frames 2500 --inputs build/f3a/landmakrj-inputs/1p-vs-cpu.txt --dump-every 100 --out build/f3a/run-1p`

Observed:
- `0x401f6e` (word): value 0 across all 2500 frames (min 0, max 0).
- `0x401f53` (byte): 0 in attract; 0x41 during 1P character select (bit 0 = P1 active, bit 6 = P1 select); 0x01 in 1P gameplay.
- Trigger `prev_word == 0 && now_word == 1`: never fires (0 times).

---

## Scenario C: 2P Versus (Game Select menu)

Script: `build/f3a/landmakrj-inputs/2p-versus.txt`
```text
# 2P versus: P1 inserts coin, P2 inserts coin, P1 starts, moves right to 2P VS (option 2), confirms
700+20 coin
720+20 p2 coin
800-1300 mash keys=start period=45
1400+5 right
2000-end mash keys=b1 period=45 seed=5
```

Command:
`uv run f3 analyze run --game landmakrj --frames 2075 --dump-start 2060 --dump-every 1 --inputs build/f3a/landmakrj-inputs/2p-versus.txt --out build/f3a/run-2p`

Transition series around trigger:
| Frame | 0x401f6e (vs_latch) | 0x401f53 (players_sel) | 0x401f7e (join_latch) | Screen / Event |
|---|---|---|---|---|
| 2062 | 0x0000 | 0x41 | 0x0000 | Game Select menu: '2P 対戦' confirmed, confirm animation playing |
| 2063 | 0x0000 | 0x41 | 0x0000 | Game Select menu confirm animation |
| 2064 | 0x0000 | 0xc3 | 0x0000 | `sub_09e962` at 0x09ea0c sets bit 1 and bit 7 in 0x401f53 |
| 2065 | 0x0001 | 0xc3 | 0x0000 | `sub_08edde` at 0x08edfe writes `move.w #1, -$6092(a5)` (**TRIGGER FIRES**) |
| 2066 | 0x0001 | 0xc3 | 0x0000 | `round_task` calls `sub_08ee06` -> spawns Character Select task `sub_09cd04` |
| 2075 | 0x0001 | 0xc3 | 0x0000 | Transitioning to Character Select |
| 2200 | 0x0001 | 0xc3 | 0x0000 | 'PLAYER SELECT' screen fully active with 1P and 2P cursors |

Screenshot at trigger frame: `build/f3a/run-2p/dumps/frame_2065/rendered.png` shows Game Select tiles fading out, P2 credit deducted (CREDIT 1 displayed).

---

## Scenario D: P2 Challenger mid 1P game

Script: `build/f3a/landmakrj-inputs/p2-challenger.txt`
```text
# Challenger: P1 starts 1P match, plays, P2 inserts coin and joins mid-game at frame 2180
700+20 coin
740+20 p2 coin
800-1400 mash keys=start period=45 seed=7
1200-end mash seed=3
2180+5 p2 start
```

Command:
`uv run f3 analyze run --game landmakrj --frames 2310 --dump-start 2295 --dump-every 1 --inputs build/f3a/landmakrj-inputs/p2-challenger.txt --out build/f3a/run-challenger`

Transition series around trigger:
| Frame | 0x401f6e (vs_latch) | 0x401f53 (players_sel) | 0x401f7e (join_latch) | Screen / Event |
|---|---|---|---|---|
| 2179 | 0x0000 | 0x01 | 0x0000 | 1P active gameplay |
| 2180 | 0x0000 | 0x83 | 0x0001 | P2 presses start: `sub_08f0d6` sets bits in 0x401f53 and `0x401f7e = 1` |
| 2240 | 0x0000 | 0x83 | 0x0001 | 'HERE COMES A NEW CHALLENGER' banner displayed on screen |
| 2298 | 0x0000 | 0x83 | 0x0001 | Challenger banner finishing |
| 2299 | 0x0000 | 0x83 | 0x0001 | Challenger banner finishing |
| 2300 | 0x0001 | 0x83 | 0x0000 | `sub_08f79a` at 0x08f7ee writes `move.w #1, -$6092(a5)` (**TRIGGER FIRES**) |
| 2301 | 0x0001 | 0x83 | 0x0000 | Challenger task launches Character Select task `sub_09cd04` |
| 2310 | 0x0001 | 0x83 | 0x0000 | Mosaic wipe transition to Character Select |
| 2450 | 0x0001 | 0x83 | 0x0000 | 'PLAYER SELECT' screen active |

Screenshot at trigger frame: `build/f3a/run-challenger/dumps/frame_2300/rendered.png` shows the end of the Challenger banner right as character select is launched.

---

## Scenario E: P2 join during Character Select

Script: `build/f3a/landmakrj-inputs/p2-charsel-join.txt`
```text
# Scenario E: P2 joins during 1P character select
700+20 coin
740+20 p2 coin
800-1400 mash keys=start period=45 seed=7
1200-end mash seed=3
1450+5 p2 start
```

Command:
`uv run f3 analyze run --game landmakrj --frames 1600 --dump-start 1445 --dump-every 1 --inputs build/f3a/landmakrj-inputs/p2-charsel-join.txt --out build/f3a/run-charsel-join`

Transition series around trigger:
| Frame | 0x401f6e (vs_latch) | 0x401f53 (players_sel) | 0x401f7e (join_latch) | Screen / Event |
|---|---|---|---|---|
| 1448 | 0x0000 | 0x41 | 0x0000 | 1P Character Select active ('PUSH START BUTTON' blinking for P2) |
| 1449 | 0x0000 | 0x41 | 0x0000 | 1P Character Select active |
| 1450 | 0x0001 | 0xc3 | 0x0000 | P2 start pulse: `sub_08ee06` at 0x08ee2c sets `0x401f6e = 1` (**TRIGGER FIRES**) |
| 1451 | 0x0001 | 0xc3 | 0x0000 | Both players active in Character Select |
| 1460 | 0x0001 | 0xc3 | 0x0000 | CREDIT 0 (P2 credit deducted), 2P character cursor active |

Screenshots: `dumps/frame_1449/rendered.png` shows 'PUSH START BUTTON' top right with Credit 1; `dumps/frame_1450/rendered.png` shows 'PUSH START BUTTON' cleared; `dumps/frame_1460/rendered.png` shows CREDIT 0 and 2P character select joined.

---

## Scenario F: Full match lifecycle, round resolution, and second match refire

Script: `build/f3a/landmakrj-inputs/two-matches-full.txt`
```text
# Two full matches in one session:
# Match 1:
700+20 coin
720+20 p2 coin
800-1300 mash keys=start period=45
1400+5 right
2000-11000 mash keys=b1 period=45 seed=5
# Match 1 finishes at frame 11150, continues expire at 12300, returns to attract.
# Match 2: insert coins and start
13000+20 coin
13020+20 p2 coin
13100-13400 mash keys=start period=45
13600+5 right
13700-end mash keys=b1 period=45 seed=7
```

Observed lifecycle:

1. **Match 1 start**: Trigger fires at frame 2065 (`0x401f6e` transitions 0 -> 1).
2. **Match 1 gameplay**: `0x401f6e` stays 1 continuously from frame 2065 to frame 11399 (9,335 consecutive frames). The trigger `prev_word == 0 && now_word == 1` does NOT refire anywhere in this span.
3. **Round 1 resolution**: At frame ~8000, Round 1 ends; `0x401f5c` becomes 2 (P2 won round 1); at frame 8200, `0x401f6c` (`p2_wins`) increments from 0 to 1.
4. **Round 2 resolution**: At frame 10950, Round 2 ends; `0x401f5c` becomes 2 (P2 won round 2); at frame 11150, `0x401f6c` increments from 1 to 2.
5. **Match winner / match over**: At frame 11150, P2 reaches target round wins ((total_rounds >> 1) + 1 = 2); `sub_09242c` sets `0x401f70 = 0xffff` (match over) and sets bit 4 in `0x401f53` (value 0x13, P1 defeated).
6. **Match cleanup and latch clear**: At frame 11400, `round_task` at PC 0x08e5f2 executes `clr.w -$6092(a5)`:
   - `0x401f6e` clears from 1 to 0!
   - `0x401f6a` (P1 wins) and `0x401f6c` (P2 wins) clear to 0 (at 0x08e5e6 / 0x08e5ea).
   - `0x401f70` (match over) clears to 0 (at 0x08e5ee).
   - `0x401f5c` (round winner) clears to 0.
7. **Continue countdown**: Frames 11400..12300 show continue countdown (`game_active = 0x19`, `0x401f6e = 0`).
8. **Return to attract**: At frame 12300, continue expires and the game returns to attract mode (`game_active` drops, `0x401f6e = 0`).
9. **Match 2 start**: At frames 13000..13020, coins are inserted. At frame 13100, P1 starts. At frame 13600, right moves to '2P 対戦'. At frame 13700, B1 confirms.
10. **Match 2 trigger refires**: At frame 13766, `sub_08edde` sets `0x401f6e` from 0 to 1 (**TRIGGER REFIRES FOR MATCH 2**).

Exact frames around Match 2 trigger:
| Frame | 0x401f6e (vs_latch) | 0x401f53 (players_sel) | Note |
|---|---|---|---|
| 13764 | 0x0000 | 0x41 | Game Select menu confirm animation |
| 13765 | 0x0000 | 0xc3 | `sub_09e962` at 0x09ea0c sets player active bits |
| 13766 | 0x0001 | 0xc3 | `sub_08edde` at 0x08edfe writes `move.w #1, -$6092(a5)` (**FIRES AGAIN**) |
| 13767 | 0x0001 | 0xc3 | Committing to Match 2 Character Select |
