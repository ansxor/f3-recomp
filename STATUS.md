DONE

# Phase 4 — Land Maker 1v1 rollback netplay

Worktree `wt/netplay`, branch `netplay`, based on `checkpoint-5-sound-default`.
Implementation, eight-seed full-length impaired acceptance, snapshot proof,
frontend verification and preserved parity gates are complete.
Local commits only; no pushes or edits to integration/other worktrees.

## Delivered

- Fixed-size, pointer-free canonical machine snapshots; zero save/load allocations. Full native CPU/lazy flags, scheduler, RAM, EEPROM, both sound CPU modes, OTIS/DSP/DUART/gain, queued PCM, retained FDP and GameVideo state. Expanded presentation is preserved, not reconstructed approximately.
- Headless rollback core: 16-frame prediction window, 17 preallocated full snapshots, delayed local input, repeat-last prediction, earliest-dirty resimulation, bounded stalls, full-state checksums every 60 confirmed frames, explicit desync errors.
- Confirmed-only audio: speculative PCM is replaced on correction and never emitted twice. State/frame/PCM/sample-count parity against a single-machine reference is enforced by the oracle.
- Dependency-free Go UDP two-player relay and native client transport: ROM/build/settings/EEPROM/initial-state handshake, fixed input histories, redundant packets and separate cumulative input/checksum ACKs, ping/frame pacing, persistent finite-match verdict, room/packet/rate bounds, safe fail-closed disconnect/rejoin.
- Frontend flags `--netplay-server`, `--netplay-room`, `--netplay-player`, `--netplay-delay`; local-player controls include synchronized coin/start/service and shared test. Live SDL title reports connection, RTT, frame advantage and rollback. Offline defaults remain native main/native sound/game video.
- `tools/netplay_oracle.cpp`, `tools/run_netplay_oracle.py`, shared deterministic versus schedule, build fingerprint generation, [docs/NETPLAY.md](docs/NETPLAY.md), README and ABI inventory.

## Exercised evidence

- **8 impaired seeds × 20,000 frames** (1, 2, 3, 5, 8, 13, 21, 34), two actual native client processes through the actual Go server, with **80 ms RTT, ±20 ms jitter, 3% loss, 3% reorder**. Both clients exactly match reference state CRC, framebuffer CRC, confirmed PCM CRC and sample count. Impaired runs exercised 877–988 rollbacks per client, including depth 16. Four additional clean-network seeds (1, 2, 3, 5) also pass 20,000 frames each.
- **240 varied-N/K snapshot checks**: 4 seeds × 2 sound drivers × N={0,1000,2000,3000,4000,5000} × K={1,7,16,31,97}. Exact immediate restore/state CRC, RAM/palette/graphics/control/shared memory, pixels, PCM and complete sound trace bytes. Every replay includes host-clock perturbation; every save/load allocation count is zero.
- **120 additional dense snapshot/performance checks** (both drivers, N every 100 through 5900) and **15 expanded-presentation checks** (scale 2, border 48 through frame 2200), all exact and allocation-free.
- Late input (120 ms withholding at frame 1500): event-local depth-16 correction after a 49.23 ms full-window stall. One-second pause: 942.63 ms full-window stall then recovery. Both recovered to exact reference state/PCM at frame 3000. Build mismatch explicitly rejected; peer kill detected by transport, not harness watchdog.
- Delay endpoints 0 and 8: actual two-client 2000-frame matches equal references. Additional MMIO smoke covers both player ports, coin/start/service, shared test OR/release and nibble mapping. Native-core wrong frame-60 checksum produces explicit local/remote `DESYNC` diagnostics.
- Actual Cocoa/Metal frontend pairs completed 1800 and 3600 impaired frames with equal states and pixels. The 1800-frame WAVs are byte-equal. Live window/title captured. Separate seed-1 captures visibly show two-player selection and sustained two-board versus play.
- Fresh MAME parity: **25/25** native frames, 600–3480 at step 120, **zero differing pixels**; frame-600 RAM byte-equal. Default 3600-frame WAV byte-equal to root `build/coverage-final.wav`. **51,507,335 main native blocks; zero interpreter fallbacks**.
- The existing seed-5 single-player gameplay harness completes 6000 frames with the prior 80,338,232 native-block count, zero fallback, framebuffer CRC `04ea93ae` and 3,029,425 stereo sample frames.
- `f3rt-check` and `go test -race ./...` pass. Parser regressions rejected malformed trailing bytes/flags/input bits/frame overflow and the server/client input-count mismatch after failing-before fixes. Relay tests cover endpoint/session isolation, identity rejection and lost final-verdict recovery after a finisher leaves.

## Performance and limits

Apple M5, 10 logical CPUs, 32 GiB RAM, Darwin 25.2/arm64, Release build; no other netplay verification jobs during the isolated benchmark. Native snapshot: **4,231,509 bytes**, default snapshot ring **68.6 MiB**. Oracle sound snapshot: 4,231,724 bytes.

| Native operation | Mean | p95 | Maximum |
| --- | ---: | ---: | ---: |
| Save | 113.0 µs | 183.5 µs | 290.9 µs |
| Load | 99.8 µs | 176.8 µs | 217.3 µs |
| Normal step | 3620.7 µs | 3989.4 µs | 9848.4 µs |

Native mean step throughput **276.2 FPS**; oracle **146.3 FPS**. Including replay and snapshot copies, `(D+1)*(step+save)+load <= 16.667 ms` permits native rollback depth **3 mean / 2 p95 / 0 worst observed**, oracle **1 / 1 / 0**. A 16-frame history is not 16-frame real-time headroom. Periodic state CRC, network, SDL and scheduling add cost; stalls/catch-up can take multiple display intervals. Confirmed-only audio adds latency.

Snapshots require the same build/configuration and emulation-thread ownership; they are not portable/untrusted save files. Frontend netplay cold-boots erased EEPROM, requires strict native CPU/sound and native-size game video, and rejects persistence/diagnostic modes. No hot-resume state transfer, encryption, account authentication, spectators or anti-cheat. Nonmatching builds fail closed. Main CPU interpretation is never a netplay fallback; existing FDP rendering of unsupported boot frames is separate.

Full commands, protocol, state inventory, determinism reasoning, evidence and deployment limits: [docs/NETPLAY.md](docs/NETPLAY.md). Artifacts remain local under ignored `build/`; ROMs/generated code/captures are not committed.
