# Land Maker 1v1 rollback netplay

Target: Japanese Land Maker 2.01J (`landmakrj`), strict statically recompiled main CPU and native sound-ROM execution through emulated devices. Netplay is opt-in. Offline defaults and the native 320×232 renderer are unchanged. This is a two-player input relay, not a server-side emulator, streaming service, or state-transfer protocol.

## Build and play

Use the same build and ROMs on both clients. The Go relay needs Go 1.22 or newer and only the standard library; the game uses the existing CMake/SDL3 dependencies. The native client transport uses POSIX sockets and was exercised on macOS; this is not a Windows client port.

```sh
# From wt/netplay; use ../roms/landmakr instead in a repository-root checkout.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR=../../../roms/landmakr
cmake --build build --target landmakr f3rt-netplay-oracle -j 6
(cd netplay/server && go build -o ../../build/netplay-server .)

# Server; expose this UDP port to both clients.
build/netplay-server -addr 0.0.0.0:9000

# Player 1 and player 2, in separate processes/machines:
build/landmakr --netplay-server SERVER:9000 --netplay-room example --netplay-player 1 --netplay-delay 2
build/landmakr --netplay-server SERVER:9000 --netplay-room example --netplay-player 2 --netplay-delay 2
```

Omit `--netplay-player` for automatic assignment. Both clients must agree on delay (0–8 frames; default 2). Room codes contain 1–32 ASCII letters, digits, `_` or `-`. The window title shows room state, assigned **zero-based** slot, RTT, frame advantage, simulated/acknowledged frame and last rollback depth. CLI player numbers are **one-based**. Escape quits; errors are reported on stderr and in an SDL dialog.

Controls address the **local assigned player**, not a fixed P1 port:

| Key | Synchronized input |
| --- | --- |
| Arrows | Up/down/left/right |
| Z / X / C | Buttons 1 / 2 / 3 |
| 1 or 2 | Local player's start |
| 5 or 6 | Local player's coin |
| F1 | Local player's service input |
| F2 | Shared test input, OR of both players |

Focus loss releases the local input word through the same delayed network path. No coin, service or test key bypasses synchronization. P2 directions/buttons occupy the high **nibble**, not bits 8–11, of the F3 control ports. Start is port-0 bit 12/13, service bit 9/10; EEPROMIN holds test bit 1 and coin bits 4/5.

### Cold boot and versus entry

Every session starts at frame zero with an erased 93C46 image (64 words of `0xffff`); the ROM performs its own factory initialization. Persistent `--eeprom`, CPU fallback, sound tracing, renderer diagnostics, expanded presentation and oracle sound are rejected in frontend netplay. This avoids mutable local configuration not covered by the handshake. Both peers still execute the normal boot; no RAM patches, injected credits or save-state boot shortcut.

Insert a coin on each client, then press each local start. The opponent joins through the game's normal challenge/character-selection flow. The reproducible oracle schedule inserts P1 coin at sampled frames 700–719 and P2 coin at 740–759; start pulses run through 800–2399, at `f % 90 < 5` for P1 and `45 <= f % 90 < 50` for P2. Independent seeded button streams begin at 1200. With delay 2 these inputs are applied two frames later. Seed 1 captures show two-player character selection at frame 1500 and independently changing two-board play at 2400 and 5700. At the latter two points RAM `0x401f53 & 3 == 3` and word `0x4078f6 == 2`. These are observed indicators, not a complete mode-enumeration specification.

## Machine snapshot contract

`Machine::state_size()`, `save_state(span<uint8_t>)`, `load_state(span<const uint8_t>)` and `state_crc()` live in `include/f3rt/machine.hpp`. Storage is fixed after sound/video configuration. Save/load require exact-sized buffers, frame boundaries and exclusive emulation-thread access. They allocate nothing. `state_crc()` lazily allocates scratch once; the rollback core hashes its already saved snapshots instead.

Native sound + native-sized game video is **4,231,509 bytes** per snapshot; oracle sound is **4,231,724 bytes**. The default prediction window is 16 frames with **17 preallocated full snapshots**, approximately 68.6 MiB for the native snapshot ring. The headless core accepts windows up to 32. No compressed/delta-state dependency chain is involved.

Canonical records are packed, explicitly initialized, pointer-free, host-endian state for the **same configured build**, not a portable file format. Load only internally generated states: the Machine API checks size, not arbitrary snapshot contents, and rollback never accepts state bytes over the network. See [CPU ABI history](developer/ABI-CHANGES.md#machine-snapshot-contract-and-canonical-state-inventory) and `runtime/state_io.hpp` for the field inventory.

Included:

- Main CPU architectural registers, lazy condition flags, cycles, dispatch deadline, stopped/halted state; scheduler deadlines, IRQs, watchdog and frame.
- Main/palette/graphics/control/shared RAM, inputs, coin counters/lockouts, EEPROM words and serial transaction state.
- Native sound CPU state, or pointer-free Musashi sound state with process-local callbacks rebound on load; sound work RAM, reset/debt and clock accumulators.
- All 32 OTIS voices/filter histories/banks, ES5510 program/GPRs/2 MiB delay RAM and ALU/MAC/RAM pipeline, DUART timers/transmitters, MB87078 latches/gain and mixer phase.
- Logical queued PCM followed by deterministic zero padding. Ring indices are rebased on load, so equal playback queues do not hash differently because of consumer history.
- FDP buffered sprites, trails, framebuffer and retained display state even when GameVideo is selected. Derived line-cache keys are invalidated on restore.
- GameVideo tile/text/glyph maps, staging/submitted/current sprites, line/scene records, native pixels/plane and both optional expanded presentation buffers.

Excluded: immutable ROM/dispatch tables, pointers/callbacks, native-block/fallback/video diagnostic counters, trace sinks, SDL textures/audio queues, sockets and host clocks. Diagnostic counters intentionally count resimulation work; they are not machine state.

## Rollback, audio and time

`runtime/netplay.cpp` contains the SDL-independent core. Simulation frame `f` consumes one word per player; local sampling at `f` is assigned to `f + delay`. The initial delay frames are neutral and known. Missing remote words repeat the most recent used word. A changed late word marks the earliest dirty frame; synchronization reloads that frame's pre-step snapshot and resimulates through the previous frontier. A repeated equal word does not cause rollback. Conflicting actual inputs are errors.

The confirmed frontier is exclusive: all frames below it have both actual inputs. Prediction never exceeds the configured window; an older-than-retained correction is an error, never a guessed recovery. A stalled peer therefore freezes at a bounded frontier and can resume without losing history. Input history is a fixed 1024-frame ring, separately tagged by absolute frame.

Resimulation runs the real CPUs, renderers and sound devices. It does **not** publish SDL frames, WAV samples or external trace records a second time. Per-frame speculative PCM is retained and replaced during correction; only confirmed PCM enters the bounded host-output queue. This trades audio latency for exact output: no duplicated speculative sound or heuristic crossfade. A consumer must drain that queue; overflow is explicit. Finite runs stop simulation at the requested frame, wait for confirmation, exchange final CRCs and await the server's persistent completion verdict.

CRC32 covers the full canonical machine snapshot after every 60 confirmed frames. It excludes host output and diagnostics but includes device queues/state. Mismatch stops with frame/local/remote CRC diagnostics. Inputs and checksum delivery are independently acknowledged.

Simulation uses integer machine clocks only. Source review found no wall clock or random-device sampling in machine, renderer or audio stepping. The deterministic input schedule uses a fixed 64-bit LCG. Snapshot proof perturbs host timing with a 1 ms sleep during every replay while comparing state, RAM, pixels, PCM and sound trace bytes. Network nonces, impairment RNG, RTT, socket deadlines, SDL pacing and window titles use host time **outside** simulation state.

Frontend pacing follows the native `6671500 / (432 * 262)` Hz cadence, not a fabricated 60 Hz clock. Peer frame reports are aged by transport delay; the frontend yields when ahead of the last peer report by more than two frames plus estimated one-way RTT in frames. Headless unthrottled testing deliberately saturates the rollback window. Performance reporting uses the stricter **16.667 ms** 60 Hz budget; a 16-frame history is not a promise that a 16-frame correction fits one display interval.

## UDP protocol, version 1

All multibyte wire values are **big-endian**. Maximum datagram size is 1400 bytes. Every datagram begins with:

| Offset | Bytes | Meaning |
| --- | ---: | --- |
| 0 | 4 | ASCII `F3NP` |
| 4 | 1 | Version 1 |
| 5 | 1 | Packet type |
| 6 | 2 | Flags |
| 8 | 8 | Server-assigned session ID; zero before start |
| 16 | 1 | Sender slot 0/1; `0xff` for server |
| 17 | 3 | Reserved, zero |

Types: 1 JoinReq, 2 JoinWait, 4 JoinReject, 5 MatchStart, 6 GameData, 7 Heartbeat, 8 Leave, 9 MatchTerminated, 10 MatchComplete. Type 3 is reserved. Full definitions are in `netplay/server/protocol.go`; the C++ implementation is `runtime/netplay_transport.cpp`.

JoinReq payload: nonce `u64`, requested slot `u8` (0 auto, 1/2 player), delay `u8`, room length `u8`, zero-padded room `[32]`, identity `[72]`. MatchStart echoes nonce, gives slot and delay, two reserved bytes, and peer identity. The session ID is in the header. Retransmitted joins are idempotent; MatchStart verifies nonce and identity before the client leaves frame zero.

Identity: seven `u32` ROM-region CRCs (main, sprites, sprite high planes, tiles, tile high planes, sound, samples), build SHA-256 `[32]`, settings `u32`, EEPROM CRC `u32`, initial-state CRC `u32`. Settings encode schema revision, native sound/GameVideo selection and delay. The build hash covers runtime/ABI/lifter/game configuration, generated main/sound C, compiler/platform and configured flags; it refreshes when source dependencies change. It is deliberately conservative: different builds are rejected rather than claimed cross-platform deterministic. The server distinguishes ROM, build, settings, EEPROM, initial-state and delay mismatches.

GameData payload, in order:

1. Simulated frontier, cumulative input ACK, cumulative checksum ACK, ping ID, echoed peer ping ID: five `u32` values. `0xffffffff` means no ACK.
2. Flags `u16`, first input frame `u32`, input count `u16`, contiguous input words `[count]` of `u16`.
3. Checksum count `u16`, then `(frame u32, CRC u32)` pairs.
4. If finish-request flag 1 is set, final confirmed frame `u32` and state CRC `u32`. Flag 2 acknowledges peer finish information; the server verdict, not this peer flag, permits exit.

Input bits 0–3 are U/D/L/R, 4–6 buttons, 7 start, 8 coin, 9 service, 10 test. Other bits are invalid. Client packets carry at most 128 contiguous input words and eight pending checksums (receiver bound 32). Unacknowledged prefixes are repeated every >=8 ms; ACKs remove only received prefixes. A frame-tagged 2048-input client history protects against duplicates/reordering and detects mutation. Checksum ACKs are separate from input ACKs so loss cannot silently skip a comparison. Ping echo samples a smoothed RTT. UDP sources are restricted to the connected relay endpoint; session, sender, lengths, counts and input masks are checked before applying packet contents.

MatchComplete carries `(frame u32, CRC u32)`. The server retains this verdict and resends it when either finisher retries, including after the other peer leaves. This prevents one successful peer from making a lost final verdict unrecoverable.

### Lifecycle and deployment limits

The relay holds no ROMs, snapshots or authoritative game state. It pairs exactly two slots, authenticates session traffic against the recorded UDP endpoint and routes validated input/checksum packets. An existing nonce may rebind its endpoint; a fresh process cannot join a running match. After an abort/8-second peer timeout, the old session is terminated and both clients must cold-start a new session. Old-session packets cannot terminate a replacement match. Use a fresh room code after completion or wait for the 30-second finished-room expiry. No hot-resume/state transfer or silent reconnection.

Bounds: 1024 rooms, 4096 tracked IP limiters, 500 packets/second per IP with burst 250, 2048 queued impaired packets. Waiting rooms expire after 60 seconds. Client handshake limits are 10 seconds without server replies and 120 seconds total waiting for an opponent; connected peer silence fails after 8 seconds. IPv4/IPv6 UDP is supported by address resolution. Room codes and session IDs are routing identifiers, **not authentication**. There is no encryption, anti-cheat, account system, spectator protocol or cryptographic protection against an on-path attacker; use a trusted relay/network.

Impairment affects server sends and uses a seeded PRNG:

```sh
build/netplay-server -addr 127.0.0.1:9000 \
  -rtt 80ms -jitter 20ms -loss .03 -reorder .03 -duplicate .01 -seed 5
```

RTT sets one-way base delay to half the value; `-delay` can set it directly. Jitter is uniform ±the requested duration, clamped at zero. Reordering adds delay to selected packets; duplication sends a second copy. A seed makes impairment decisions reproducible for the same arrival sequence, not host process scheduling or packet delivery order across separate executions. The deadline heap uses strict deadline/sequence ordering.

## Reproducible verification

```sh
(cd netplay/server && go test -race ./...)
python3 tools/run_netplay_oracle.py --suite snapshot --frames 6000 \
  --seeds 1 2 3 5 --sound-driver all --log-dir build/netplay-snapshots
python3 tools/run_netplay_oracle.py --suite baseline --frames 20000 \
  --seeds 1 2 3 5 --sound-driver native --log-dir build/netplay-baseline
python3 tools/run_netplay_oracle.py --suite impaired --frames 20000 \
  --seeds 1 2 3 5 8 13 21 34 --sound-driver native --log-dir build/netplay-impaired
python3 tools/run_netplay_oracle.py --suite cases --log-dir build/netplay-cases
```

The runner starts the compiled Go server, waits for its actual readiness log and launches **two OS client processes**. Every requested seed receives the full frame count; reference inputs have the identical delay convention. Successful finite matches require equal final state CRC, framebuffer CRC, confirmed PCM CRC and sample count for both clients and the reference. The default impaired suite uses 80 ms RTT, 20 ms jitter, 3% loss and 3% reorder.

Snapshot proof covers N=0,1000,2000,3000,4000,5000 and K=1,7,16,31,97 for each seed/driver, checks immediate restore CRC, byte-compares RAM/palette/graphics/control/shared memory, framebuffer, PCM and complete sound traces, and instruments `new` during save/load. Traces are temporary, uniquely named and removed after each replay.

Edge scenarios with native sound: withhold local input for 120 ms at frame 1500; pause all peer pumping for 1000 ms; corrupt the build hash; kill a peer after confirmed progress reaches 1000. Late/stall checks measure corrections whose first dirty frame lies in the injected frame range, or sustained full-window stalls in that range, rather than accepting unrelated lifetime counters. Both recovery cases must equal the reference's state and confirmed audio. Disconnect must be reported by transport, not the harness watchdog. Relay tests additionally cover stale sessions, endpoint spoofing, identity mismatches, malformed packets and completion-verdict retransmission after one finisher leaves.

Native-only main execution is enforced in all harness paths. Renderer fallback for unsupported boot frames is the existing FDP path, not CPU interpreter fallback. Existing MAME/checkpoint parity remains an independent gate; netplay peer agreement alone cannot prove correct emulation.

## Measured results and limits

Release build on Apple M5, 10 logical CPUs, 32 GiB RAM, Darwin 25.2/arm64.
The isolated run below had no other netplay verification jobs running.
It measures 60 saves, 60 loads and 4800 normal mid-game steps per driver:

```sh
build/f3rt-netplay-oracle --mode snapshot --frames 6000 --seed 5 \
  --sound-driver all --snapshot-interval 100 --snapshot-k 1
```

| Sound driver / operation | Mean µs | p95 µs | Maximum µs |
| --- | ---: | ---: | ---: |
| Native save | 113.0 | 183.5 | 290.9 |
| Native load | 99.8 | 176.8 | 217.3 |
| Native normal step | 3620.7 | 3989.4 | 9848.4 |
| Oracle save | 109.4 | 126.2 | 181.1 |
| Oracle load | 94.0 | 106.0 | 109.0 |
| Oracle normal step | 6833.1 | 7172.2 | 11011.0 |

Step includes machine execution, rendering, audio drain and the harness's PCM
checksum; traces are disabled for these samples. Native mean throughput is
**276.2 FPS**, oracle **146.3 FPS**. Save/load allocation counts are zero.

For rollback depth D, the estimate includes D replay frames plus the next
normal frame and snapshot copies:

`(D + 1) * (step_us + save_us) + load_us <= 16666.67`

Native affordable D is **3 mean / 2 p95 / 0 worst observed**. Oracle is
**1 / 1 / 0**. This is headroom estimation, not a hard real-time guarantee:
periodic full-state CRCs, networking, SDL and OS scheduling can add cost.
In particular, a full 16-frame correction takes multiple display intervals.
Input delay, pacing and bounded frontier stalls handle that situation;
the implementation does not hide it by dropping simulation or audio.

Exercised acceptance:

- Four native baseline seeds (1, 2, 3, 5), each 20,000 frames. Both clients equal
  their single-machine reference's state, framebuffer and confirmed PCM.
- Eight full-length impaired seeds (1, 2, 3, 5, 8, 13, 21, 34), each 20,000
  frames, under 80 ms RTT / ±20 ms jitter / 3% loss / 3% reorder. Both clients
  equal the reference's state, framebuffer, PCM and sample count. Per-client
  rollback counts were 877–988, with actual depth-16 corrections.
- 240 varied-N/K snapshot checks across four seeds and both sound drivers,
  plus 120 dense-sampling checks in the isolated performance run. Exact state,
  memory, pixels, PCM and sound trace agreement; zero save/load allocations.
- Expanded presentation (scale 2, border 48): 15 additional exact snapshot
  checks through frame 2200, including fallback and supported game frames.
  Snapshot size 6,547,797 bytes. Frontend netplay still deliberately requires
  native-size presentation.
- Late-input recovery recorded a 49.23 ms full-window stall and depth-16
  correction attributable to the withheld frame range. The 1000 ms pause
  produced a 942.63 ms bounded full-window stall. Both recovered to state
  `f66a012a`, PCM `04f19777`, 1,514,712 stereo sample frames at simulation 3000.
- Real build-mismatch rejection and peer-kill timeout passed; delays 0 and 8
  additionally matched references for 2000 frames each. A direct native-core
  smoke injected the wrong frame-60 checksum and observed explicit local/remote
  `DESYNC` diagnostics. MMIO smoke checked both players' coin/start/service,
  shared test OR/release and button/direction nibble mapping.
- Actual Cocoa/Metal frontend peers completed 1800 and 3600 frames under
  impairment with matching state/frames; the 1800-frame WAVs were byte-equal.
  Live window capture showed ready slot, RTT, frame advantage and rollback
  status. Headless seed captures separately verified human-versus-human play.
- Fresh MAME capture: **25/25** frames (600–3480, step 120), zero differing
  pixels; frame-600 RAM byte-equal. Default native 3600-frame WAV byte-equal to
  the `checkpoint-5-sound-default` reference `build/coverage-final.wav`.
  51,507,335 main native blocks, **zero interpreter fallbacks**.
- The existing single-player seed-5 gameplay harness also completed 6000
  strict-native frames: 80,338,232 native blocks (the recorded pre-netplay count),
  zero fallback, framebuffer CRC `04ea93ae`, 3,029,425 stereo sample frames.
- `f3rt-check` and `go test -race ./...` passed. Malformed-wire and input-count
  boundary regressions were observed failing before their parser fixes.

Final reference/peer agreement for the impaired 20,000-frame runs:

| Seed | State CRC32 | Confirmed PCM CRC32 |
| ---: | --- | --- |
| 1 | `b9715bee` | `106b0e7b` |
| 2 | `d7e729ba` | `0e4cabc2` |
| 3 | `127ec5e5` | `4a491d11` |
| 5 | `99e5910b` | `94bee9b2` |
| 8 | `4b5925a8` | `7f7dda63` |
| 13 | `3aceeda3` | `754ea8b9` |
| 21 | `7d7d98da` | `0b76f433` |
| 34 | `b6288683` | `8fde76c0` |

Logs/captures live under ignored `build/netplay-*`, `build/versus-proof`,
`build/versus-images`, `build/native-parity` and `build/mame-reference`.
No ROMs, generated C or captured game/audio data are committed.
