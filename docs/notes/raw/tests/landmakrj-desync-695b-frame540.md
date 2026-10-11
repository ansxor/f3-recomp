---
source_url: "build-landmakrj/f3rt-tool netplay-replay (failed, see 2) + harnesses np_replay/np_patch/np_fix"
ingested: 2026-10-11
sha256: cde622ab1be9d6adcb1c83412d15d7ed01dea4ef2cec0c170a19f7ce099ace88
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "harness against build-landmakrj/libf3rt.a, git 849cfef + uncommitted"
---
# Land Maker desync at match frame 540 (session 695b9e187e06116c): unsynced player-struct residue

Sources copied here: `landmakrj-695b-p0-info.json`, `landmakrj-695b-p1-info.json` (bundle info), harnesses `landmakrj-np_replay.cpp.txt` (replay + diff), `landmakrj-np_patch.cpp.txt` (patch trigger state, re-diff), `landmakrj-np_fix.cpp.txt` + `landmakrj-np-fix-matrix.sh.txt` (full-match matrix with/without fix). Bundles: `~/Library/Application Support/f3-recomp/f3rt/netplay-desync/20261011T150337Z-695b9e187e06116c-p0|p1`. Relay log `/tmp/np-logs/695b9e187e06116c.json`: `crc compared 9, mismatch_count 1, {frame 540, crc_p1 890864, crc_p2 538156404}`, inputs identical, no protocol errors.

## 1. Bundle facts

| | p0 (host) | p1 (joiner) |
|---|---|---|
| trigger / edge / sync / start machine frame | 1144 / 1209 / 1209 / 1209 | 1490 / 1555 / 1555 / 1555 |
| frames_between_sync_and_start | 0 | 0 |
| lobby_hold_frames / ms | 652 / 11239 | 536 / 9373 |
| stall_count / clock_resyncs / audio_queue_drops | 0 / 2 / 11 | 0 / 3 / 9 |
| hash phase at 540 | 2 (491 B) | 2 (491 B) |

The mismatching report is phase 2, not phase 1: the previous report (480) was phase 1 (109 B, equal), so the gate flipped between 480 and 540. Replaying (below) shows `0x401f82=0x57` from match frame 516 and `0x40210e` 0 -> 1 at match frame 522 on both peers. Inputs are identical for all 540 frames (`diff` of the two inputs.txt: equal; only P2 presses, frames 131 onward, none after 200 that matter). The previous fix (zero frames between sync and start) held: frames_between_sync_and_start = 0 on both.

## 2. Replay check

`build-landmakrj/f3rt-tool netplay-replay --bundle <p0> --bundle <p1> --rom-dir ...` fails at load: `trigger.f3s is 1701710 bytes, this build's machine state is 3468025`. Cause (tool problem): the frontend runs GPU game video + enhanced audio; `create_strict_machine` in the tool builds no game video and reference audio. Sizes measured (`np_size`): gpu=0/ref 3468025, gpu=0/enhanced 1037370, gpu=1/ref 4132365, gpu=1/enhanced 1701710 (= the bundle). Fix for the tool: `create_strict_machine(rom, set, true, 1)` then `m->audio->set_backend(Audio::Backend::Enhanced)`; I did this in the harness.

With that configuration each bundle replays to its own recording:

```
VERIFY ...-p0 frame=540 phase=2 len=491 bytes=yes crc=0x000d97f0 recorded=0x000d97f0
VERIFY ...-p1 frame=540 phase=2 len=491 bytes=yes crc=0x20139d74 recorded=0x20139d74
```

So the live frontend is deterministic relative to the replay; the desync is real divergent state, not nondeterminism.

## 3. First divergence

```
FIRST_DIVERGENCE frame=522 phase 2/2 len 491/491
  0x40206c: A=00 B=ff
  0x40206d: A=00 B=f4
  0x402078: A=ff B=fb
  0x4020f6: A=00 B=01
```

Per-frame watch (`np_watch`): `0x4020f4.l` is `0xb4` on A and `0x15d` on B from match frame 1 until it is cleared at frame 525 (`sub_093462` `clr.l -$5f0c(a5)` at 0x09372a, then counts up from 1 at 527: equal on both); `0x40206c.w` (written only by `sub_093be4` `move.w d0,$20(a2)`, 0x093c72) is `0000` vs `fff4` through 540; `0x402078` (written by `sub_093b06` at 0x093b7a/0x093b92 and `sub_09f9c4`) `ff` vs `fb` until 527 and equal after. The whole block 0x401f40..0x40212f already differs at match frame 1 in 19 bytes (`401fd8 401fd9 401fe1 401ffb 402033 402061 402069 40206c 40206d 402078 40207a 40207b 40207d 402089 4020a3 4020db 4020f6 4020f7 402127`): the player-structure residue from the different pre-trigger attract/charsel paths (trigger 1144 vs 1490). Phase 1 hashes only 109 B, so it stays hidden; when `0x40210e` flips to 1 (first instruction of `sub_093462`, before it initialises the structures) the 491 B hash includes residue that init has not yet overwritten. `0x4020f6` is the high half of a 32-bit counter whose low byte `0x4020f7` was already in the exclusion list.

## 4. Fix and verification

Fix: sync the whole gameplay block instead of the selected sub-ranges. Replace the sync ranges `{0x401f40,68}, {0x40203c,4}, {0x4020e4,4}, {0x40210e,2}` by one range `{0x401f40, 496}` (0x401f40..0x40212f). Keep `{0x400090,16}`, `{0x4000cb,13}`, `{0x40fc58,2}`. Payload 527 B, blob 528 B (bump `sync_blob_version` to 0x04).

Verification 1, bundles (`np_patch`: after loading each trigger.f3s copy host RAM bytes lo..hi into the joiner, replay 540 frames, compare hash and, separately, the whole block incl. excluded bytes):

```
copy 401f40 40212f   NO HASH DIVERGENCE through 540 frames (phase 2)   (and no whole-block difference at all)
copy 401f40 401f83   HASH DIVERGE at frame 522 (this is today's sync)
copy 401f40 40203f   HASH DIVERGE at frame 522
```

Verification 2, full matches (`np_fix`, GPU+enhanced machine, mashed inputs, 6000 frames, hash as it is today; `FIXSYNC=1` copies the 496 B block host -> joiner after apply_sync):

```
cfg 1 300 2 0  (scripts: host trigger 1600 hold 300 / joiner trigger 1450)
  FIXSYNC off: DESYNC at completed frame 345 crcA=e12e5935 crcB=21cefc60  0x402078: A=ff B=fb
  FIXSYNC on : NO DESYNC through 6000 frames; final crc=11801539
cfg 2 0 1 123 (reverse roles)
  FIXSYNC off: DESYNC at completed frame 345  0x402078: A=fb B=ff
  FIXSYNC on : NO DESYNC through 6000 frames; final crc=11801539
cfg 0 0 1 0 / 1 0 0 400 / 4 50 3 700: no desync with or without the fix (residues happened to agree), final crc 11801539 with the fix.
```

The harness reproduces the same byte (0x402078) as the real bundle without the fix.

[INFERENCE] The hash region lists (109 B phase 1 / 491 B phase 2) were left unchanged in these runs. If `sync_hash_region` derives phase 1 from `sync_ranges`, the phase 1 hash grows to 527 B; not tested.
