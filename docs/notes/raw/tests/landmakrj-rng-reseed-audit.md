---
source_url: docs/notes/log.md + docs/site/developer/netplay/index.md
ingested: 2026-10-11
sha256: 1f2a7e9d4d962e55ee634bd5879cb5caf752b43a6c926709c038a894098fba9f
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository record of runs (secondhand: 2P 18,000-frame watch run and 16,000-frame two-peer match; artifacts not retained); source doc deleted after ingest; tests/test_netplay.py"
---
<!-- excerpt: docs/notes/log.md lines 135-137 (2026-10-11 entry: RNG reseed keyed on round index; 0x400826 consumer audit) -->
## [2026-10-11] update | landmakrj RNG reseed keyed on round index; 0x400826 consumer audit
Audit of `0x400826` (xref plus a 2P 18,000-frame watch run): readers are `sub_001468` (LCG; idle loop `$19f8` via `sub_001174` advances it, the drift source), `sub_093462` @0x0935d4 (copies the word into BOTH players' bag seeds, once per round, ~12 frames after the stage pick), `round_task` @0x08e470 (stage step `((rng&3)+1+prev)&7` into 0x401f54), `sub_08e84c` (sound pick), `sub_0915d2` (tile/palette), `sub_0a04d0` (dice roll `rng&0x7f + table[0x4000d0][0x401f54] + 0x401f7a`, script opcode 2; role unconfirmed, Haiku read as CPU AI), and a ~40-entry `dc.l $1468` callback table (unresolved). `sub_093a1c` swaps the word with the player seed and restores it, so after init it does not consume the global. Wins sum `0x401f6a+0x401f6c` is 0/1/2 at the stage picks at frames 2631/4983/11043 and is updated 2 frames before each pick. `on_match_frame` now keys on `seed ^ (wins_sum * prime)` instead of `match_frame`: a pure function of the server seed and synced RAM, rollback-safe. Side effect: the word is constant within a round, so the `sub_0a04d0` roll and cosmetic readers repeat per round. Verified: tests/test_netplay.py 6 passed, 16,000-frame two-peer match equal sync_hash, no DESYNC. Not observed: a round-end inside that run, and rounds that end without a winner.
Touched: games/landmakrj/netplay/netplay.cpp, docs/site/developer/netplay/index.md, docs/notes/log.md.

<!-- excerpt: docs/site/developer/netplay/index.md lines 59-97 (Land Maker hook description) -->
## The Land Maker hook

`games/landmakrj/netplay/netplay.cpp` implements every field.

**Versus latch.** `in_versus` returns true when the 16-bit word at `0x401f6e` equals 1.

**Start-state sync.** `capture_sync` writes a 528-byte blob. Byte 0 is the sync version (`0x04`). The following 527 bytes are these ranges, in order:

| Blob offset | Game address | Length | Content |
| --- | --- | --- | --- |
| 1 | `0x400090` | 16 | Credits and coin chute subsystem (credits counter `0x400090`, partial coins `0x400092`/`0x40009e`, coin debounce `0x400096`/`0x40009a`). Overwritten on connect with the host's values so continue and challenger permissions stay identical. |
| 17 | `0x4000cb` | 13 | Operator settings and options (coinage, free play, difficulty/2P enable, cabinet, coin mode, continue, demo sound, free-play config, option words). Overwritten on connect with the host's values. Screen flip (`0x40013e`) is visual/presentation only and is intentionally not synced. |
| 30 | `0x401f40` | 496 | The whole gameplay block `0x401f40..0x40212f`: the mode block (game state, players-active word `0x401f53`, stage word, round winner and wins, versus latch `0x401f6e`, match-over word `0x401f70`, join latch `0x401f7e`, phase word `0x401f82`), both player structures, board and piece queues, the piece generator RNG seeds (`0x40203c`, `0x4020e4`) and the task latch `0x40210e`. |
| 526 | `0x40fc58` | 2 | Rounds setting. |

The 527-byte set is the whole gameplay block plus the credits block `0x400090..0x40009f`, the operator settings block `0x4000cb..0x4000d7` and the rounds word.

`apply_sync` requires exactly 528 bytes and version `0x04`. Otherwise it throws `Invalid sync blob size or version`. It then writes the four ranges back. The seed argument is not used. `[INFERENCE]` What the session does when the hook throws was not traced.

**Per-frame reseed.** `on_match_frame` writes the 32-bit RNG word at `0x400826` before every lockstepped frame:

```
round_index = read16(0x401f6a) + read16(0x401f6c)   // round wins P1 + P2
key = seed ^ (round_index * 0x100000001b3)
write32(0x400826, splitmix32(key))
```

`splitmix32` is the splitmix64 finalizer truncated to 32 bits. `seed` is the shared session seed, drawn by the relay server (`netplay/server/state.cpp`, `random_u64()`) and sent to both peers in MATCH_START. The RNG word is not in the sync blob, because the reseed sets it. The key depends on no session counter, only on the seed and synced RAM, so a machine snapshot restores it.

The value is constant within a round. The win counters are updated about 2 frames before `round_task` (`0x08e470`) picks the stage and about 12 frames before `sub_093462` seeds both players' piece generators from this word, so each round gets a new stage step and new bags. Every other reader of `0x400826` also sees the same value for the whole round, including the roll in `sub_0a04d0` (`0x0a04d0`), whose role is unconfirmed. A round that ends without a winner would repeat the previous round's key. `[INFERENCE]` The notes list only winners 1 and 2.

**Desync hash.** `sync_hash` is ungated: CRC32 over 522 bytes from the first report, in this order:

- `0x401f40` to `0x40212f` (496 bytes), excluding five bytes: `0x401f85`, `0x401fa1`, `0x402033`, `0x4020db` and `0x4020f7` (491 bytes). The notes classify these as cosmetic or residue.
- The other synced ranges: credits (16), operator settings (13) and the rounds word (2).

The gameplay block is fully synced, so the player-structure residue that the old two-phase gate (`0x401f82 >= 0x50 && 0x40210e == 1`) used to hide is equal on both peers from match frame 0. The hook reports `phase = 1` always. Bundles written by builds before `hash_layout` 2 hold the gated 491-byte block (phase 2); `netplay-replay` compares those against the leading 491 bytes of the current region.

`Machine::state_crc` is not used. It differs between the two copies on every frame, because of sound CPU, ES5510, DUART, palette and graphics RAM residue, the frame counter at `0x400142` and the timers at `0x40032c`.
