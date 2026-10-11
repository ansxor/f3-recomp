# Netplay for developers

**What you will learn:** how netplay is split across the relay server, the runtime session and the per-game hook; what each hook field does in the Land Maker implementation; the workflow used to make a game netplay-ready; and how to test it.

Netplay is two-player, lockstep and relay-only. The server forwards control messages, a start-state sync blob and per-frame inputs. It never runs game code and never sees game memory. Both game copies run the same frames from the same start state. The per-game hook decides what the start state is and what each side must reseed or hash.

For the wire format, message IDs and server rules, read the [protocol page](/developer/netplay/protocol).

## Parts

| Part | Path | Role |
| --- | --- | --- |
| Relay server | `netplay/server/` | C++ (Yojimbo, cpp-httplib, cpp-yyjson). Single port (TCP token endpoint, UDP game socket), encrypted rooms, JSON logs. Built by `make netplay-server`. |
| Wire and session | `runtime/netplay/` | Protocol v3, Yojimbo client (`client.cpp`), lockstep session state machine (`session.cpp`). Compiled into `f3rt`. |
| Public API | `include/f3rt/netplay.hpp` | `SessionConfig`, `Session`, `SessionState`, `EndReason`, `step_lockstep_frame`. |
| Game hook type | `include/f3rt/game_netplay.hpp` | `GameNetplay`, returned by `f3rt::game_netplay()`. |
| Default hook | `runtime/stubs/netplay.cpp` | Returns `nullptr`. Used when `games/<set>/netplay/` has no sources. |
| Game hook source | `games/<set>/netplay/*.cpp` | Per-game hook. CMake globs this directory for the selected `F3_GAME`. Land Maker: `games/landmakrj/netplay/netplay.cpp`. |
| Frontend | `runtime/frontend/frontend.cpp`, `ui.cpp`, `settings.cpp` | Settings, lobby, versus trigger, CLI flags, status text and overlay. |
| Headless client | `tools/commands/netplay.cpp` | `f3rt-tool netplay`, for scripted two-process runs. |
| Build identity | `cmake/generate_build_identity.cmake`, `cmake/build_identity.hpp.in` | Writes `build_identity.hpp`. |

## Session flow

Host:

1. Connects to relay: acquires 2048-byte Netcode connect token over HTTP (`GET /token?v=3`) and connects via UDP.
2. Channel 0 (reliable): sends HELLO (identity and name), receives WELCOME.
3. Channel 0: sends CREATE_ROOM (`input_delay`, `flags`), receives ROOM_CREATED with the room code. Flag bit 0 is set when `capture_sync` is configured.
4. The joiner sends JOIN_ROOM. The server sends MATCH_START to both players.
5. If `capture_sync` is set, the host calls it and sends SYNC_BLOB (1 to 2048 bytes as block data) before GO.
6. Server sends GO once the sync blob has been relayed (or immediately if no sync blob is required). Both sides enter Running.

Joiner:

1. Acquires connect token over HTTP (`GET /token?v=3`) and connects via UDP.
2. Channel 0: sends HELLO, receives WELCOME. Sends JOIN_ROOM, receives MATCH_START.
3. If `apply_sync` is set, the joiner waits for SYNC_BLOB_FWD, applies the blob, and then waits for GO.
4. Server sends GO. Both sides enter Running.

**Invariant: no machine frame between the sync point and GO.** `capture_sync` / `apply_sync` run on the exact `in_versus` edge frame (the frontend restores its lobby-trigger snapshot and runs input-0 frames to that edge, without `lobby_hold_frame` or `on_match_frame`). From that moment until GO the frontend runs no frames at all, so match frame 0 is exactly the synced state on both peers. Running pinned hold frames in that window (a different count per peer) desynced the match. The frontend logs `NETPLAY_RUNNING ... frames_between_sync_and_start=N` and `NETPLAY_INVARIANT_VIOLATION` if N is not 0; `f3rt-tool netplay` prints `NETPLAY_INVARIANT frames_between_sync_and_start=0` and exits 1 otherwise. Both values are recorded in desync bundles.

In Running, each side sends its inputs over Channel 1 (unreliable) with redundant multi-frame history. A frame runs only when both inputs for that frame are known. Each side sends a CRC every 60 frames on Channel 0. At the end, the frontend sends the result and then BYE.
## Hook contract

The frontend and the headless tool build one `SessionConfig` from `f3rt::game_netplay()`. The signatures below are taken from `include/f3rt/game_netplay.hpp`.

| Field | Signature | Called by | Rules |
| --- | --- | --- | --- |
| `in_versus` | `bool (*)(Machine &)` | Frontend loop, both players | Return true only in 2-player versus. A non-null `in_versus` makes netplay available for the game. |
| `capture_sync` | `size_t (*)(Machine &, std::span<uint8_t>)` | Host, at MATCH_START | Write 1 to 2048 bytes and return the count. The session resizes the buffer to the returned count. See the pitfalls below. |
| `apply_sync` | `void (*)(Machine &, std::span<const uint8_t>, uint64_t seed)` | Joiner, once, after SYNC_BLOB_FWD | Must bring the joiner to the host's start state. Throw on a bad blob. |
| `on_match_frame` | `void (*)(Machine &, uint64_t seed, uint32_t match_frame)` | Both players, once per match frame, before `run_frame` | Make only deterministic writes here. Called from `step_lockstep_frame` in `runtime/netplay/session.cpp`. |
| `sync_hash` | `uint32_t (*)(Machine &)` | Both players, every 60 frames | Replaces `Machine::state_crc` for desync checks. Identical gameplay states must give identical values. |
| `lobby_trigger` | `bool (*)(Machine &)` | Frontend loop, both players | Optional: opens the frontend lobby on false -> true edge (e.g. at menu confirmation before the scene switch). Falls back to `in_versus` if null. |
| `lobby_hold_begin` | `void (*)(Machine &)` | Frontend loop, once at lobby trigger | Optional: records reference timer/counter values for the lobby hold state. |
| `lobby_hold_frame` | `void (*)(Machine &)` | Frontend loop, each held frame | Optional: pins game logic state (scene, timers, flags) to trigger values while animations and music continue. |
| `lobby_cancel` | `void (*)(Machine &)` | Frontend loop, on cancel/close | Optional: uncommits menu selection and resets timer so player returns to Game Select menu. |
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

**Per-frame reseed.** `on_match_frame` writes the 32-bit RNG word at `0x400826`:

```
key = seed ^ (match_frame * 0x100000001b3)
write32(0x400826, splitmix32(key))
```

`splitmix32` is the splitmix64 finalizer truncated to 32 bits. `seed` is the shared session seed. The RNG word is not in the sync blob, because the per-frame reseed sets it.

**Desync hash.** `sync_hash` is ungated: CRC32 over 522 bytes from the first report, in this order:

- `0x401f40` to `0x40212f` (496 bytes), excluding five bytes: `0x401f85`, `0x401fa1`, `0x402033`, `0x4020db` and `0x4020f7` (491 bytes). The notes classify these as cosmetic or residue.
- The other synced ranges: credits (16), operator settings (13) and the rounds word (2).

The gameplay block is fully synced, so the player-structure residue that the old two-phase gate (`0x401f82 >= 0x50 && 0x40210e == 1`) used to hide is equal on both peers from match frame 0. The hook reports `phase = 1` always. Bundles written by builds before `hash_layout` 2 hold the gated 491-byte block (phase 2); `netplay-replay` compares those against the leading 491 bytes of the current region.

`Machine::state_crc` is not used. It differs between the two copies on every frame, because of sound CPU, ES5510, DUART, palette and graphics RAM residue, the frame counter at `0x400142` and the timers at `0x40032c`.

## Pitfalls from the code

- **`capture_sync` with a bad blob.** The host sets flag bit 0 in CREATE_ROOM before it calls `capture_sync`. If the blob is empty or over 2048 bytes, the host sends nothing. `[INFERENCE]` The server then never sends GO, and the match stalls. This follows from the code and was not run.
- **`apply_sync` without `capture_sync`.** The host sends flag 0, so no blob is relayed. `[INFERENCE]` The joiner waits for SYNC_BLOB_FWD and never registers. This follows from the code and was not run.
- A hook field and its pair must be set together. The frontend gives both players the same hook, so a mismatch comes from a hook that sets one field and not the other.
- On desync the session writes `DESYNC frame=N p1=0x... p2=0x...` to stderr. The server log records the same frame.
- **Desync bundle.** `MatchRecorder` (`runtime/netplay/recorder.cpp`) keeps `trigger.f3s` (one `save_state` at match frame 0), every applied input pair, and an 8-entry ring of the hashed bytes (`sync_hash_region`, 522 bytes per report, no allocation). It saves no full state during play: a per-report `save_state` cost 3 to 4 ms in the frontend. On DESYNC it writes `info.json`, `hashed.bin`, `inputs.txt` and `trigger.f3s`. `info.json` records the machine configuration (`machine_gpu`, `audio_backend`, `machine_state_bytes`): GPU game video and the enhanced audio backend change the state layout (the frontend's state is 1,701,710 bytes, the default headless tool's 3,468,025). `f3rt-tool netplay-replay --bundle DIR --dump-state FRAME out.f3s` builds the matching machine, regenerates the state at any match frame through `run_lockstep_frame`; with two `--bundle` arguments it reports the first diverging frame and addresses. Bundles that predate the config fields are matched by state size, or with `--gpu 0|1 --audio enhanced|reference`; the tool errors if the build cannot construct that configuration. `f3rt-tool netplay` takes the same `--gpu`/`--audio` flags. A game's hook must provide `sync_hash_region` and `sync_hash_addresses` for this.

## Workflow for making a game netplay-ready

Land Maker is the worked example. The steps below are the order used to reach the current hook. The raw evidence is in `docs/notes/raw/tests/landmakrj-differential-sync-experiment.md` and the routine note `docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md`.

1. **Trigger.** Find the versus latch: the word that goes 0 to 1 when the game enters 2-player versus. For Land Maker it is `0x401f6e`. The routine note lists three places that set it: mode select, a second player joining during character select, and a challenger joining mid-game. The routine note's status is `hypothesis`. Other games need their own discovery. The notes record trigger frames of 2065 (mode select), 1450 (mid-character-select join), and 2300 (mid-game challenger) in the tested runs.

2. **A/B differential harness.** Run two copies of the game from different attract-mode histories so that the trigger falls on different frames. The experiment used trigger frame 2065 for Path A and frame 2945 for Path B. Compare the two states at the trigger. The raw states differ, as expected. The oracle is a full `Machine::save_state` copy (about 3.47 MB). Copying it made the two copies byte-identical for 5000 frames in the notes. A full snapshot cannot be sent in SYNC_BLOB, so it is only the reference.

3. **Find the leaks.** Copy the RAM ranges to the joiner with no reseed and check for divergence. The 550-byte set without reseed stayed identical until step 1380, then diverged in the stage theme byte at `0x401f55`. The piece RNG diverged at step 1516. The notes trace the cause to the 68EC020 idle loop at ROM `$19f8`. It spins on spare cycles while it waits for VBlank, and it calls ROM `$1468`, which advances `0x400826`. Spare-cycle counts differ between the two copies, so the RNG phase drifts. A CPU cycle-count sync was also tried and threw `CPU cycle clock moved backwards`.

4. **Make the mechanic deterministic per frame.** Reseed the RNG at `0x400826` from the shared seed and the match frame, in `on_match_frame`. With that reseed, the 550-byte set stayed identical for 8000 frames in the notes. This removes the idle-loop phase from the result.

5. **Shrink the sync set, then check different attract paths.** Keep only the RAM the game needs to start the match. The notes report the 450-byte set (spec 6B) identical for 8000 frames and the 78-byte set (Spec 6C) identical for 5000 frames from one path pair. That did not hold for other pre-trigger paths: see step 7. The shipped set is now 527 bytes (v4).

6. **Hash the synced state, ungated.** Hash the gameplay region from the first report. The notes found 11 bytes that differ in the 496-byte block before round init; five are excluded (cosmetic counters and attract-mode timers). The remaining differences were player-structure residue, which is why step 5's smaller set failed (see below): once the whole block is synced, no gate is needed. A gate hides the problem until it opens, and then the residue leaks into gameplay at that frame.

7. **Sync the whole block, not selected ranges.** The shipped v3 set (109 bytes, gated hash) desynced a real match at frame 540 (`docs/notes/raw/tests/landmakrj-desync-695b-frame540.md`). The two peers had reached the trigger through different attract/charsel paths, which left different player-structure bytes (`0x40206c`, `0x402078`, `0x4020f4..f7` and others) in `0x401f40..0x40212f`. They stayed outside the synced sub-ranges and leaked into gameplay when the gate flipped. v4 syncs all 496 bytes. Regression: `test_netplay_residue_paths_stay_in_sync` runs two such paths; against the v3 sub-ranges it reports `DESYNC frame=30` (`--crc-every 30`, checked by hand).

Use `sync_hash`, not `state_crc`, in the hook. Test with `f3rt-tool netplay --crc-every 1` so a desync is reported on the frame where it starts.

## Adding a game

1. Create `games/<set>/netplay/<name>.cpp` and return a `GameNetplay` from `f3rt::game_netplay()`. Start from `games/landmakrj/netplay/netplay.cpp`.
2. Implement the workflow above for the game. Each step has a pass criterion: a trigger that fires once per match, a differential run that stays identical across different pre-trigger paths, a sync set that fits in 2048 bytes, and an ungated hash with zero desyncs.
3. Run the checks in the Testing section.

## Build identity

The build hash is generated by `cmake/generate_build_identity.cmake`. CMake passes it the list `F3_SIMULATION_SOURCES`. That list contains the game TOML, the machine and ROM headers, the generated `program.c` and `program.bin`, and `sound_program.c`. The script takes the SHA-256 of each file, sorts the entries by relative path, and hashes the combined list. The result goes into `build_identity.hpp` as `build_hash`, together with `default_game_id`.

The server compares `game_id`, the seven ROM CRCs and `build_hash` from the two HELLO messages. It does not compare `player_name`.

## Frontend integration

- Settings keys are `netplay_server`, `netplay_name` and `netplay_delay`. `settings.cpp` validates them: server non-empty and at most 256 bytes, name 1 to 24, delay 1 to 8.
- CLI options (listed in `--help`): `--netplay-server HOST:PORT` (default `127.0.0.1:7845`), `--netplay-auto host|join:CODE` and `--netplay-name NAME` / `--netplay-delay 1..8`. They override the saved settings for one run.
- `--netplay-auto` takes `host`, `host:file:PATH`, `join:CODE` or `join:file:PATH`. With `host:file:PATH` the frontend writes the room code to `PATH`. With `join:file:PATH` it polls `PATH` for up to 5 seconds (50 times, 100 ms apart) and reads the code from it.
- Stdout lines `NETPLAY_ROOM <code>` and `NETPLAY_RUNNING match_frame=N` are emitted for scripts.
- When no session is active, the 0 to 1 edge of `in_versus` opens the lobby. `--netplay-auto` starts host or join.
- The lobby shows a warning that state sync is not yet implemented only for games that have neither `capture_sync` nor `apply_sync`. Land Maker has both, so it does not show the warning.
- Input scripts with memory poke rules are rejected while netplay is active. `--inputs` with pokes plus `--netplay-auto` or a netplay hook throws.

## Testing

- `runtime/tests/netplay.cpp` builds as `f3rt-test-netplay`. GoogleTest discovery uses the prefix `runtime-netplay.`. Run `ctest -R runtime-netplay`. This needs `BUILD_TESTING` and GoogleTest. `[INFERENCE]` The exact ctest invocation was not run.
- `tests/test_netplay.py` is a pytest file. It needs a ROM directory (`F3_ROM_DIR`, `ROM_DIR` or `roms/landmakrj`), an `f3rt-tool` in `build-landmakrj/` or `build/`, and `build/netplay-server`. The server is started on `127.0.0.1:0`. The file has four tests:
  - `test_netplay_poke_script_rejected`: a poke script is rejected with a message containing "poke".
  - `test_netplay_end_to_end_3000_frames`: a two-process match for 3000 frames.
  - `test_netplay_differential_sync_5000_frames`: host at trigger 2065 and joiner at trigger 2945, with scripted inputs for 5000 frames. It checks that both `TRIGGER` lines appear, that there is no DESYNC, that both `FINAL sync_hash` values are equal, and that the `state_crc` values differ. The differing `state_crc` values show that the two copies came from different attract paths.
  - `test_netplay_differential_sync_negative_control`: the same setup for 500 frames, with `--disable-apply-sync` on the joiner. It expects a DESYNC or a non-zero exit.

  No `add_test` in `CMakeLists.txt` refers to this file, so it is not part of ctest. These tests were not run for this page.
- Server unit tests: `ctest --test-dir build -R '^netplay-relay\.'`.
- Headless client: `f3rt-tool netplay --server HOST:PORT --host` or `--join CODE`. Useful flags:
  - `--inputs FILE` runs before the trigger. Pokes are rejected.
  - `--until-trigger` runs `--inputs` locally until `in_versus` goes 0 to 1, then prints `TRIGGER frame=N [sync_hash=0x...]`.
  - `--match-inputs FILE` supplies the match frames after the trigger. Pokes are rejected.
  - `--delay N` (1 to 8), `--frames N` (default 3000), `--crc-every N` (default 60).
  - `--disable-apply-sync` skips `apply_sync` on the joiner, as a negative control.
  - `--rom-dir DIR` and `--set SET` (default `landmakrj`).

  Run `f3rt-tool netplay --help` for the full list. Output lines are `ROOM <code>`, `FINAL frame=N sync_hash=0x... state_crc=0x...` and `DESYNC frame=N p1=0x... p2=0x...`. The exit code is 0 only when the run reaches `--frames` and reports no desync.

## Known limits

- The Land Maker sync covers the blob ranges above. Other state, such as sound and animation timing, can differ between the copies. The desync check covers the synced gameplay block minus five cosmetic bytes, plus credits, operator settings and the rounds word.
- The relay is the only path. There is no direct connection, and there is no reconnect.
- The relay encrypts and authenticates every packet per client using Netcode (ChaCha20-Poly1305 AEAD). Connect tokens are signed with the relay's private key.
- A single relay process supports at most 32 concurrent rooms (`--max-rooms`, default 32) due to `yojimbo::MaxClients = 64`.
- Single port (default 7845) is used for both TCP (token endpoint) and UDP (game socket).
- SYNC_BLOB is limited to 2048 bytes, so a full snapshot cannot be sent.
- The host's input delay sets the delay for the match.
