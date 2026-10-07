# ImGui and versus-only netplay: implementation and evidence

This records the ImGui/versus-only cutover, not the former cold-start protocol. The short entry point is [NETPLAY.md](../NETPLAY.md). Source contracts are in `include/f3rt/{machine,netplay,netplay_session,netplay_transport}.hpp`, `runtime/{machine,netplay,netplay_session,netplay_transport}.cpp`, `runtime/state_io.hpp`, and `netplay/server/{protocol,room,server}.go`. The target ROM is Japanese Land Maker 2.01J. Peer agreement alone is not proof of emulation parity.

## Frontend and default invariants

F1 toggles the menu, F12 saves a PNG, F3 is service and F2 is test. Escape closes an open menu, otherwise quits. Solo simulation pauses while the menu is open; network simulation continues with local P1 gameplay input neutral. Offline P1/P2 profiles are independent, while network input uses the local P1 profile for its assigned slot. P1 defaults to arrows/Z/X/C/1/5/F3/F2. P2 defaults only to start/coin on 2/6. No gamepad bindings are implicit.

Preferences are saved only by explicit **Save preferences**. `--config` selects another configuration; command-line values win over saved values. The default directory comes from `SDL_GetPrefPath("f3-recomp", "f3rt")`; `states/` and `screenshots/` sit alongside the config. Offline slots 0–9 require strict-native execution and compatible build, ROMs and geometry. Volume changes host output, not hardware state or peer checksums.

GPU postprocessing is Off by default. Off/CRT/User are host-only transforms; CPU presentation requires a GPU backend to use them. CLI selection is `--postprocess off|crt|user`, with `--user-shader PATH`. Metal uses `.metal`, entry `f3_postprocess`; Vulkan uses `.spv`, entry `main`. Texture/sampler slot 0 and a float4 dimensions/scale/time uniform form the user-shader interface. Examples are `runtime/renderer/shaders/user_transform.metal` and `.frag`. Failed live reload preserves the last valid shader. Native pixels/checksums are unchanged; postprocess screenshots include the transform, the menu does not. Presentation controls are live only where the UI identifies them as such.

Accurate/native audio remains the default, and the default unopened-menu emulation path remains native 320×232. Expanded presentation, different local EEPROMs and different prior solo histories do not prevent netplay pairing. Strict-native main execution is required; sound tracing and interpreter fallback are rejected. Legitimate FDP renderer fallback during unsupported boot frames is unrelated to CPU interpreter fallback.

## Lifecycle: local history to a fresh versus handoff

Exactly two slots and exactly one host are required. Host is a snapshot authority role, not synonymous with P1: a host may occupy P2. Host delay is authoritative (0–8, default 2); player/room/server/delay settings alone do not start a session. F1 Host/Join or `--netplay-host`/`--netplay-join` makes the client ready.

`Session` moves through Lobby → Preparing (host) / WaitingSnapshot (guest) → Sending / Accepting → Playing → Finishing → Local. Local simulation continues in the lobby and on the waiting guest. After pairing, the host's preparation uses normal coin, start and button-1 confirmation pulses, not credits/RAM injection. Coin/start edges are staggered in a 200-frame cycle; confirmation pulses handle interrupted tutorial/selection. Preparation errors after more than 3600 frames rather than waiting forever. The guest can have unrelated local history and EEPROM state before handoff.

### Exact Japan 2.01J predicate

At a native frame boundary:

- Versus latch: big-endian RAM word `$401f6e == 1`.
- Both active players: `($401f53 & 3) == 3`.
- `versus_match_active` requires both conditions.
- `versus_handoff_ready` additionally requires `($401f53 & 0xc0) != 0`: at least one player is still choosing a character. The other may already have confirmed.

Low active flags alone also occur in tutorial/demo. `$401f54` is the random stage selector (0–7), not a mode discriminator; `$401f54 == 8` is not an entry predicate. The former `$4078f6` heuristic is superseded. ROM code at `$8efcc` sets the versus latch after testing low flags 3; stage selection at `$8e464` consults it and `$8e5f2` clears it. Controlled single-start P1/P2 histories showed tutorial low flags 3 with latch 0 and real solo low flags 1/2 with latch 0. Repeated Start can enter versus even without a P2 Start, so the oracle now uses genuine one-start local prehistory (2400/3200 frames).

The host drains solo PCM, neutralizes input ports, saves a canonical sync snapshot and freezes while sending. The guest saves its prior full local state, loads the host sync state into its own configured presentation geometry, verifies the handoff predicate and empty PCM queue, then acknowledges the loaded canonical CRC. The relay releases the start barrier only for that transfer/CRC. Host snapshot creation plus guest load acknowledgment constitute the both-loaded boundary. No rollback is constructed before the barrier; its frame 0 is relative to the host's handoff machine frame, not cold boot.

### Confirmed exit and disconnect

Every simulated frame records whether the active-versus predicate became false. That speculative observation alone does not end the match. As actual inputs advance the exclusive confirmed frontier, the first confirmed exit restores the snapshot at that boundary and discards speculative post-match frames/unpublished PCM. Natural end is therefore neither a timer nor a guessed RAM heuristic. Finite capture limits also require full confirmation.

The session exchanges confirmed frame/canonical CRC completion information and waits for the relay's persistent completion verdict. Return to Local restores the retained confirmed full state, destroys transport and marks a discontinuity. Disconnect/error during rollback likewise restores the confirmed boundary. A guest that has loaded a handoff but not started rollback restores its saved pre-handoff local state on failure. A host that fails preparation/transfer remains at its reached local boundary. Start a rematch through a fresh Host/Join and fresh host snapshot; the same room is supported. There is no hot reconnect or silent state recovery.

## Full local state versus canonical synchronization state

`Machine::state_size/save_state/load_state/state_crc` retain the full local machine, including expanded presentation planes, for rollback and offline slots. `sync_state_size/save_sync_state/load_sync_state/sync_state_crc` omit the expanded GameVideo presentation buffers while retaining native pixels, rendering/trails and hardware state. Sync serialization also canonicalizes FDP scanout control fields that are rebuilt from machine controls on render. Guest sync load invalidates derived host rendering caches and rebuilds presentation under guest geometry; it does not import the host's scale or GPU resources.

Native sound/GameVideo sync size is **4,231,509 bytes**. Full native local state at scale 2/border 48 is **6,547,797 bytes**. Oracle sound adds **215 bytes** to these sizes. The default rollback window is 16, using 17 preallocated **full local** snapshots; supported windows are 16–32. Memory cost thus depends on the local configured full state, even though wire state and periodic checksums do not. There is no compressed/delta dependency chain in the rollback ring.

Save/load require exact-sized spans, a frame boundary and exclusive emulation-thread ownership; no host consumer or device reconfiguration may race them. Per-save/load allocation is zero. CRC methods allocate scratch lazily once; rollback prepares sync-CRC scratch before stepping. Records are packed, pointer-free, explicitly initialized and host-endian for compatible builds, not a portable cross-platform save format.

Included state covers main CPU architectural/lazy flags/deadlines; scheduler/IRQs/watchdog/frame; RAM and inputs/coins; EEPROM words and serial transactions; native or canonical Musashi sound CPU/work RAM/reset/debt/clocks; OTIS voices/filter histories/banks; ES5510 registers/DRAM/pipelines; DUART; MB87078; mixer phase; logical queued PCM with deterministic zero padding; retained FDP sprites/trails/framebuffer; and GameVideo maps/sprites/scene rows/native pixels/planes. Immutable ROMs/dispatch tables, pointers/callbacks, diagnostic counters, trace sinks, SDL textures/audio queues, GPU resources, sockets and wall clocks are excluded. Diagnostic counters count resimulation work and intentionally do not rewind.

### Imported-state validation

Unlike the old local-only contract, canonical snapshot bytes now cross the relay. `StateReader` validates canonical records before device loaders use their indices. Exact byte count and CRC are necessary but not sufficient. Validators reject invalid CPU lazy-flag encodings/booleans, EEPROM mode/address/bit counters, PCM counts and sound clock accumulators, OTIS voice/page indices and clock/sample-rate relationships, DSP shifts/counts/ALU/MAC/RAM selectors, gain indices, DUART timer state, and video sprite counts/flags/tile/scene selectors.

Musashi state is restricted to the board's 68000 model, address/status masks, valid internal flags/opcode/IRQ/reset state and exact 68000 timing constants (including unsigned −2 encodings). An unstarted oracle context must match the canonical zero-filled/reset-required form. Process-local callbacks/tables are rebound rather than imported. The source of exact field bounds is `runtime/state_io.hpp`; these safety checks are not cryptographic validation or a proof that a peer supplied legitimate game history. Loading is not advertised as transactional for arbitrary malformed data.

## Rollback, clocks and accurate audio

At relative frame `f`, local sampling schedules input at `f + delay`; initial delay frames are neutral/known. Missing remote input repeats its most recent used word. A changed late actual word marks the earliest dirty frame; synchronization loads its full pre-step snapshot and resimulates to the previous frontier. Equal repeats cause no rollback; conflicting actual inputs are errors. Input history is a tagged 1024-frame ring. Prediction stops at the bounded window; corrections older than retained history fail explicitly.

Real CPUs/renderers/audio execute during replay, but SDL/WAV/trace side effects are not published twice. Speculative per-frame PCM is replaced during correction; only confirmed PCM enters the bounded host-output queue. Overflow is explicit, and consumers must drain it. CRC32 every 60 confirmed frames covers **sync** state, not local expanded presentation or host audio output. Checksum and input acknowledgments are independent. Mismatch stops with frame/local/remote CRC diagnostics.

Simulation clocks are integers. Host clocks drive sockets, RTT, pacing and presentation only. Frontend cadence is `6671500 / (432 * 262)` Hz. Pacing yields when ahead by more than two frames plus estimated one-way RTT in frames; unthrottled oracle runs deliberately pressure the prediction window. A 16-frame history is not a promise that a depth-16 replay fits a display interval.

Integration also offers `--audio-backend accurate|hle`; explicit CLI audio choices override saved backend preferences, while explicit HLE plus `--sound-driver` remains an error. HLE reconciles corrections over absolute machine-frame ranges and rebases main-side clocks on non-rollback state adoption without rewinding local music/effects. Do not interpret the accurate/native PCM proofs below as HLE equivalence or exact HLE playback snapshots. Its contract and historical evidence are in [HLE-AUDIO.md](HLE-AUDIO.md), rather than duplicated here.

## UDP wire v2 and trust boundary

Multibyte wire integers are big-endian; maximum datagram size is 1400 bytes. The 20-byte header is magic `F3NP` (offset 0, 4 bytes), version **2** (4), type (5), flags u16 (6), server session ID u64 (8), sender slot (16; 0/1 or server `0xff`), and three reserved zero bytes (17).

Types are JoinReq 1, JoinWait 2, JoinAck 3, JoinReject 4, MatchStart 5, GameData 6, Heartbeat 7, Leave 8, MatchTerminated 9, MatchComplete 10, SnapshotMeta 11, SnapshotChunk 12, SnapshotAck 13, SnapshotLoaded 14, BarrierStart 15.

JoinReq is exactly 108 payload bytes: nonce u64, requested slot u8 (0 auto, 1/2 player), requested delay u8, room length u8, zero-padded room[32], identity[64], host-role u8. Identity is seven ROM-region CRC32s (28 bytes), build SHA-256 (32), and state-format u32 (4). State format encodes sync byte size in low 24 bits, schema bit 24, native sound bit 25, GameVideo bit 26 and HLE audio bit 27. EEPROM CRC, initial-state CRC, delay and presentation geometry are **not** identity fields. Build hashing is conservative and covers runtime/ABI/lifter/game configuration/generated code/compiler/platform/flags. Different ROM/build/state-format identities reject pairing, as do role/slot conflicts.

### Snapshot transfer

- SnapshotMeta is five u32 fields: transfer ID, raw size, compressed size, raw CRC32, compressed CRC32. Current transfer ID is 1; both sizes must be nonzero and at most **16 MiB**.
- Host uses zlib compression. SnapshotChunk contains transfer ID u32, index u32 and at most **1024** compressed bytes. Exact final-chunk length is required. SnapshotAck is transfer ID/index (8 bytes). Authority is host-only meta/chunks, guest-only ACK/loaded receipt.
- Metadata cannot mutate within a session. Duplicate chunks must be byte-identical; indices are bounded. The sender uses a 32-unacknowledged-chunk window, at most three sends per 8 ms opportunity, 150 ms chunk retry and 80 ms metadata/loaded retry. Transfer heartbeats run every 100 ms.
- Guest verifies compressed CRC, bounded inflate completion, exact raw size and complete compressed-stream consumption, then raw CRC. It loads and validates the canonical state before sending SnapshotLoaded (transfer ID/raw CRC, 8 bytes).
- BarrierStart is the same transfer/CRC receipt, from the server only. The relay retries it until each client's GameData proves receipt; pre-start heartbeats do not acknowledge the barrier. GameData is not accepted before release.

### Input/completion transport and relay bounds

GameData contains five u32 values (simulated frontier, input ACK, checksum ACK, ping, echoed ping), u16 flags, u32 first input frame, u16 input count, contiguous u16 words, u16 checksum count and `(frame u32, CRC u32)` pairs. Finish-request bit 1 adds confirmed frame/CRC (8 bytes); bit 2 acknowledges peer finish. Unknown flags and trailing/truncated payloads are rejected. No ACK is `0xffffffff`. Inputs use bits 0–3 U/D/L/R, 4–6 buttons, 7 start, 8 coin, 9 service, 10 shared test; other bits are invalid. Sender batches at most 128 inputs and eight checksums; receive bound is 32 checksums. Input masks, ranges/overflow, counts and acknowledgments are checked before applying contents. Unacknowledged prefixes repeat with an 8 ms minimum send interval. Transport input history is 2048 entries, maximum unacknowledged inputs 512; checksum ring is 512 entries.

MatchComplete carries confirmed frame/CRC. The relay retains/retries the verdict even if one finisher leaves. Session/recorded UDP endpoint checks reject stale-session/spoofed-source routing; fresh matches use fresh sessions/nonces. Finished-room/rematch handling includes bounded retired-nonce tombstones (120-second lifetime), so rejected joins and lost final Leave packets do not perpetually block rematches.

Relay limits: 1024 rooms, 4096 tracked IP limiters, **1000 packets/s per IP with burst 500**, and 2048 queued impaired sends. Waiting rooms expire after 60 seconds; finished/terminated room expiry is 30 seconds, subject to retired-nonce retention. Client handshake fails after 10 seconds without replies or 120 seconds total; snapshot/barrier deadline is 120 seconds from transport creation; peer silence is bounded by 8 seconds. The host preparation bound is independently 3600 frames.

The relay forwards snapshots as well as inputs/checksums; it has no ROM/emulator or authoritative simulated state. It validates routing, roles and transfer bounds, not snapshot game semantics. UDP is IPv4/IPv6 via address resolution. Room/session IDs are routing identifiers, **not authentication**. CRC is corruption detection, not authentication. There is no encryption, cryptographic integrity, anti-cheat, spectator protocol or protection against a malicious host/on-path attacker. Use a trusted relay/network even though malformed imported fields and decompression sizes are validated.

## Observed cutover evidence

Completed observations on macOS arm64 are listed below. These pre-merge cutover paths are relative to `wt/imgui-netplay`; merged integration results are recorded separately below. Generated code, ROM-derived captures and PCM remained under ignored `build/`; none are shipped as project assets.

| Scenario | Completed observation |
| --- | --- |
| Unopened default, 3600 frames | 51,507,335 native blocks; interpreter fallback 0; frame CRC `3359f200`; 1,817,655 stereo sample frames. Legitimate FDP fallback on 231 boot frames. |
| Independent MAME/default audio parity | 25/25 captures at 600–3480 step 120 exact pixels; frame-600 RAM exact; WAV byte-equal to existing `coverage-final.wav`. |
| Real-relay baseline, seed 1, 20,000 frames | Four matches, three natural ends; state `5daeba12`, PCM `3823744c`, 10,098,086 samples; rollback counts 1075/1216, maximum depths 7/4. Both clients exact against per-match `handoff.bin` references. |
| Late input / stalled peer | 120 ms withholding and 1000 ms pumping stall; 3000 frames each, exact reference. |
| Cross-presentation / delay authority | Guest scale 2/border 48 and requested delay 8 adopted host delay 2; exact reference. |
| Roles, end and rematch | Host P2, swapped slots, natural end and rematch passed. |
| Reject / disconnect | Build mismatch rejected; killed peer after confirmed 800 returned to local for 60 strict-native frames, zero fallback. Logs: `build/lifecycle-cases`. |
| Sync proof, native and oracle | 2400 frames, seed 1, boundaries 0/1/700/800/1200/2400; guest Game/Compare scale 2/border 48 and alternate GPU capture. Each: 786 checks, 372 paired frames, 372 exact local replay frames, 375,648 PCM samples; save/load/local-load allocations 0. Latest parser validators passed valid proofs. |
| Full snapshot runner | Native and oracle, 15 replay checks each: N=0/1000/2000 and K=1/7/16/31/97; exact state, RAM, pixels, PCM and sound trace bytes, with timing perturbations and zero save/load allocations. The combined full/canonical suite passed. |
| Regression suites | CTest runtime-devices/snapshot-validation/frontend-input passed; Go race suite passed. Observed failing-before/passing-after: close-capture suppression, 12 unsafe Musashi/clock contexts, rejected Join extending finished-room life, lost final Leave blocking rematch. |
| Actual Metal transforms | CRT formula maximum error 0; 371,200 user-inversion pixels at 1x/2x; invalid shader kept last valid; Off restored exact baseline; canonical CRC unchanged; fallback 0. |
| Actual F1/preferences | Metal CRT/User controls visually verified on colored scene; saved User/volume 0.32 reloaded in a new GPU process. CLI `--postprocess off --volume 75 --video-scale 2` showed Off/0.75 and 640×464 scale 2; without Save preferences the saved User/0.32/scale-1 config remained unchanged. CPU shader tab explicitly required GPU. |
| Slots/screenshots/input | Saved frame 2137, resumed 2169, load restored 2137; F12 produced PNG. Actual P1 G/P2 H remaps and virtual SDL gamepad independent coin buttons 0/1 captured/saved. No physical gamepad was connected. Metal session exited at 2167 frames after state reload, fallback 0, no audio queue drops. |
| Actual CPU/Metal network menus | Host/Join entered through ImGui after local frames 1218/2592. CPU 1x/border 0 and GPU 2x/border 48 adopted origin 1910. Menus remained open through selection into play; a UDP observer recorded 3496 actual synchronized input words (frames 2–3497), all zero despite button/direction/coin/start/service/test key input. Slots were disabled. |
| Actual UI disconnect/local resume | Host Disconnect returned both windows to local, paused at 5405/5404; closing F1 resumed local simulation (host observed 5467 before pausing again). Both processes exited normally with interpreter fallback 0 and no audio queue drops. Host/guest final frames were 5497/5607. |
| Local diagnostics after timeout | A rebuilt actual CPU frontend returned to paused local play after peer loss and displayed ping 0.0 ms, rollback depth 0 and advantage +0, rather than retaining stale network diagnostics. |
| Metal native/expanded parity | Every one of 4000 native-resolution frames matched CPU pixels. At scale 2/border 48, all 135 sampled composites matched. Both runs: 55,427,892 native blocks, interpreter fallback 0, audio CRC `d6f1b071`; 231 legitimate boot renderer fallbacks. |

### Completed baseline campaigns

Eight seeds × 20,000 synchronized frames: **160,000 frames, 40 fresh matches,
32 natural ends**. Every match used an independent reference loaded from its
actual host handoff. CRCs describe these runs, not fixed cold-boot expectations;
local pre-handoff timing may differ between runs.

| Seed | Matches / natural ends | Final state CRC32 | Confirmed PCM CRC32 |
| ---: | ---: | --- | --- |
| 1 | 4 / 3 | `5daeba12` | `3823744c` |
| 2 | 5 / 4 | `78daf2c7` | `dd228a83` |
| 3 | 6 / 5 | `360501bd` | `0182e550` |
| 5 | 5 / 4 | `e4200818` | `cedeeba9` |
| 8 | 5 / 4 | `7979d4c2` | `e8e334d8` |
| 13 | 5 / 4 | `55a0bde0` | `b4ab689b` |
| 21 | 5 / 4 | `b9c10e22` | `8a25036a` |
| 34 | 5 / 4 | `f2596652` | `af85a9ba` |

### Completed impaired campaigns

Eight seeds × 20,000 synchronized frames at **80 ms RTT, ±20 ms jitter,
3% loss and 3% reordering**, including snapshot chunks: **160,000 frames,
43 fresh matches, 35 natural ends**. Every peer matched its independent
handoff reference's state, native framebuffer, confirmed PCM CRC and sample
count. Both clients reached actual depth-16 corrections in every seed.

| Seed | Matches / natural ends | Final state CRC32 | Confirmed PCM CRC32 |
| ---: | ---: | --- | --- |
| 1 | 6 / 5 | `0c6d2d85` | `b46e953e` |
| 2 | 5 / 4 | `e3d85434` | `2e70d81c` |
| 3 | 5 / 4 | `e47828d4` | `599cd0f6` |
| 5 | 6 / 5 | `644ac45a` | `c3b6e42b` |
| 8 | 5 / 4 | `573c3ecf` | `641dd539` |
| 13 | 5 / 4 | `3934de4b` | `7c9fbbff` |
| 21 | 6 / 5 | `ca11284f` | `36082812` |
| 34 | 5 / 4 | `5970754c` | `70ba43ac` |

## Merged integration verification

The cutover was merged over integration `b71f475`, retaining the rewritten
README, `docs/developer/` evidence layout and removal of the old status document.
The integrated HLE backend is selectable from F1 and saved preferences.

- Release build: `PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages cmake --build build/hle-merge -j 8` passed. CTest passed all four suites: runtime devices, HLE audio, snapshot validation and frontend input. The rebuilt integration relay passed `go test -race ./...`.
- Default headless, 3600 frames: frame CRC `3359f200`, 51,507,335 native blocks, interpreter fallback **0**, 1,817,655 stereo frames. `cmp build/hle-merge/imgui-default.wav build/coverage-final.wav` passed.
- Full/canonical snapshot runner, 2400 frames, seed 1, native and oracle: passed. Each canonical proof again completed 786 checks, 372 paired and 372 local replay frames, 375,648 PCM samples, with zero save/load allocations. Logs: `build/hle-merge/imgui-snapshot-proof`.
- Accurate/native real-relay smoke, 3000 frames: both clients and actual-handoff reference matched state `44071b7e`, native framebuffer and PCM `0b4007b7`, 1,514,713 samples. Rollbacks 114/107, maximum depths 3/4; both returned locally. Logs: `build/hle-merge/imgui-native-relay`.
- HLE clock-adoption regression: the old implementation produced **0** stereo frames instead of 800 after adopting an earlier host clock. The repaired implementation produced exactly **800 non-silent stereo frames in both time directions**, retained replay identity after adoption, and rejected malformed mailbox index/alignment/boolean/reserved fields. Worker playback remains non-rewound; only its mapping from emulated time changes.
- HLE impaired relay, 3000 frames: 80 ms RTT, ±20 ms jitter, 3% loss/reorder, plus 120 ms input withholding. Divergent solo/EEPROM histories and a scale-2/border-48 guest adopted host delay 2. Both clients and the per-handoff reference matched state `c8f86780` and native framebuffer `7c7ecac9`. Rollbacks 465/197, depths 16/15; both produced nonzero 48 kHz PCM and returned locally. HLE playback CRCs intentionally differed; this is **not** an accurate-PCM equivalence claim. Logs: `build/hle-merge/imgui-hle-relay`.
- Actual Metal F1: selected HLE, explicitly saved preferences, restarted without an audio CLI override, and visually observed HLE selected. The restarted process exited normally at frame 3379 with `audio_backend=hle`, `sound_driver=none`, fallback **0**, and no SDL audio queue drops.
- Saved-HLE headless, 2400 frames: 35,199,233 native blocks, fallback **0**, 1,954,402 stereo frames and 2,141,703 nonzero samples. Explicit `--audio-backend accurate` and `--sound-driver native` each overrode the saved HLE selection. Explicit HLE plus sound-driver was rejected in either argument order. Netplay preference flags without Host/Join stayed local, and none of these launches rewrote saved preferences.
- HLE is explicitly rejected by accurate-PCM `snapshot`, `snapshot-proof` and `sync-proof` modes. Client/reference modes exercise its canonical command state instead.
- The merged VitePress site built successfully. Verification was on macOS arm64/Metal; Vulkan presentation and a physical gamepad were not exercised.
- Extended HLE proof against the rebuilt integration relay, 6000 impaired frames: **two handoffs, one natural end, two local returns per peer**, with fresh origins 8000 and 12047. Both matches matched independent references loaded from their actual handoff: state `7195f5a4` after 3478 frames, then `f7c42a0b` after 2522. Final native framebuffer `30f0d4c0`; rollbacks 935/408, depth 16 on both peers, fallback **0**. Non-rewound 48 kHz output remained nonzero; all 199/57 observed cancelled voices stopped within 79,791/79,789 main ticks. Logs: `build/hle-merge/imgui-hle-rematch`.

## Historical measurements: pre-cutover cold-start implementation

These are retained from the former `docs/NETPLAY.md` for engineering context only. They do **not** verify current versus handoff, sync-state omission, wire v2, menu behavior or rematch lifecycle. Historical state/PCM CRCs use the old execution/input schedule and must not be compared to the cutover campaign.

An isolated Release run on Apple M5 (10 logical CPUs, 32 GiB, Darwin 25.2/arm64) measured 60 full-state saves/loads and 4800 normal mid-game steps per driver using `f3rt-netplay-oracle --mode snapshot --frames 6000 --seed 5 --sound-driver all --snapshot-interval 100 --snapshot-k 1`:

| Driver / operation | Mean µs | p95 µs | Maximum µs |
| --- | ---: | ---: | ---: |
| Native save | 113.0 | 183.5 | 290.9 |
| Native load | 99.8 | 176.8 | 217.3 |
| Native step | 3620.7 | 3989.4 | 9848.4 |
| Oracle save | 109.4 | 126.2 | 181.1 |
| Oracle load | 94.0 | 106.0 | 109.0 |
| Oracle step | 6833.1 | 7172.2 | 11011.0 |

Steps included execution/rendering/audio drain/PCM checksum with traces disabled. Historical throughput was 276.2/146.3 FPS native/oracle; save/load allocations were zero. The estimate `(D + 1) * (step_us + save_us) + load_us <= 16666.67` gave native affordable depth 3 mean / 2 p95 / 0 worst observed, oracle 1 / 1 / 0. This was a headroom estimate, not a real-time guarantee; CRC/network/SDL/OS overhead was additional.

Historical verification comprised four 20,000-frame native baseline seeds (1/2/3/5), eight 20,000-frame impaired seeds (1/2/3/5/8/13/21/34) at 80 ms RTT, ±20 ms jitter, 3% loss/reorder, equal reference state/pixels/PCM/sample count and 877–988 rollbacks per client including depth 16. There were 240 varied-N/K snapshot checks across four seeds/two drivers plus 120 dense checks, exact RAM/pixels/PCM/traces and zero save/load allocations; 15 expanded full-snapshot checks through 2200 frames used 6,547,797-byte state. Late input produced a 49.23 ms window stall/depth-16 correction, a 1000 ms pause produced 942.63 ms stall; both recovered at frame 3000 to state `f66a012a`, PCM `04f19777`, 1,514,712 stereo sample frames. Delay-0/8 reference checks ran 2000 frames; build rejection, killed-peer timeout, explicit wrong-frame-60 DESYNC and MMIO input mapping were exercised.

Historical Cocoa/Metal peers completed 1800/3600 impaired frames; 1800-frame WAVs matched. Historical seed-5 solo gameplay completed 6000 strict-native frames: 80,338,232 native blocks, fallback 0, frame `04ea93ae`, 3,029,425 stereo sample frames. Historical `f3rt-check` and Go race checks passed, with malformed-wire/input-count fixes observed failing before correction.

| Historical impaired seed | State CRC32 | PCM CRC32 |
| ---: | --- | --- |
| 1 | `b9715bee` | `106b0e7b` |
| 2 | `d7e729ba` | `0e4cabc2` |
| 3 | `127ec5e5` | `4a491d11` |
| 5 | `99e5910b` | `94bee9b2` |
| 8 | `4b5925a8` | `7f7dda63` |
| 13 | `3aceeda3` | `754ea8b9` |
| 21 | `7d7d98da` | `0b76f433` |
| 34 | `b6288683` | `8fde76c0` |

Historical logs/captures were in ignored `build/netplay-*`, `build/versus-proof`, `build/versus-images`, `build/native-parity` and `build/mame-reference`. Cutover evidence remains in ignored build outputs. ROMs, generated code and captured game/audio data are not committed.
