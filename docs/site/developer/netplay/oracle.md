# Oracle and verification

`f3rt-netplay-oracle` checks snapshot replay and real relay lifecycle. The Python runner compares each versus match against its **actual host handoff**, not a shared cold-boot history. Recorded outcomes live in [IMGUI-NETPLAY.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/IMGUI-NETPLAY.md); commands here are instructions, not new pass claims.

## Build and run

```sh
cmake --build build --target landmakr f3rt-netplay-oracle
(cd netplay/server && go build -o ../../build/netplay-server .)
python3 tools/run_netplay_oracle.py --suite baseline --frames 20000 --seeds 1
python3 tools/run_netplay_oracle.py --suite impaired --frames 20000 --seeds 1 2 3 5 8 13 21 34
python3 tools/run_netplay_oracle.py --suite cases
```

Use the appropriate local ROM path through `--rom-dir`. Never publish ROM-derived handoff/dump/audio artifacts.

## Binary modes

| Mode | Contract |
| --- | --- |
| `snapshot` | Full local save/load and replay at multiple boundaries/depths, including allocation measurements |
| `sync-proof` | Canonical import between differing presentation geometry, exact local full-state replay, native pixels/PCM and allocation proof |
| `client` | Genuine independent one-start solo histories (P1 2400/P2 3200 frames by default), fresh host transfer, versus campaign, natural local return/rematch |
| `reference` | Replay delayed input from the saved canonical host snapshot for an independent per-match reference |

```sh
build/f3rt-netplay-oracle --mode sync-proof --frames 2400 --seed 1 --sound-driver native
build/f3rt-netplay-oracle --mode reference --frames 3000 --seed 1 \
  --initial-state build/campaign/host/match_0/handoff.bin --match-index 0 --delay 2
```

Handoff filenames above illustrate output layout; choose the actual host output directory. Host may be P2. Each rematch needs its own `handoff.bin`, origin and match-index stream.

## Binary options

| Option | Default / purpose |
| --- | --- |
| `--mode` | Select one of the modes above |
| `--set`, `--rom-dir` | `landmakrj`, built-in ROM directory |
| `--seed`, `--frames` | 12345, 20000 |
| `--server`, `--room` | `127.0.0.1:9000`, `oracle_room` |
| `--player`, `--host-player` | Client slot 1/2; host-player defaults to 1 and is independent of slot |
| `--delay`, `--window` | Host delay 2 (0–8), prediction window 16 (16–32) |
| `--prelude-frames` | Override independent solo history length (2400/3200 default) |
| `--initial-state`, `--match-index` | Canonical reference input and campaign stream index (default index 0) |
| `--sound-driver` | `native`; `oracle`; `all` for snapshots |
| `--schedule` | `versus` or `single` |
| `--video-scale`, `--video-border` | Geometry for local/canonical snapshot coverage; presentation is also supported in player netplay |
| `--snapshot-interval`, `--snapshot-k` | 1000-frame interval; default depths 1, 7, 16, 31, 97 |
| `--timeout` | 120 seconds |
| `--surface` / `--capture-surface`, `--dump-dir`, `--dump-every` | Local captures and handoff/reference artifacts |
| `--stall-at`, `--stall-ms` | Complete pause injection |
| `--withhold-input-at`, `--withhold-input-ms` | Late submission injection |
| `--observe-event-at` | Measure correction/stall around a frame |
| `--corrupt-build-hash` | Expected rejection scenario |
| `--unthrottled` | Remove yield sleep while stalled |

## Python runner

Suites are `all`, `snapshot`, `baseline`, `impaired`, `cases`. Defaults: `build/f3rt-netplay-oracle`, `build/netplay-server`, 20000 frames, delay 2, window 16, native sound and seeds **1, 2, 3, 5, 8, 13, 21, 34**. `--sound-driver all` uses both for snapshots and native for campaigns. See the [CLI reference](/reference/cli) and binary `--help` for diagnostic options.

Campaigns compare both clients to a per-match delayed reference loaded from the host's canonical handoff: canonical CRC, native framebuffer CRC, confirmed PCM CRC and stereo sample count. They require actual natural returns/rematches and no main interpreter fallback. Impairment covers snapshot chunks as well as gameplay (80 ms RTT, 20 ms jitter, 3% loss/reorder). Cases cover 120 ms withheld input, 1000 ms stall, geometry/requested-delay independence, host-P2 assignment, mismatch rejection and killed-peer local return.

Cross-presentation proof must retain native rendering/trails, validate unsafe parser fields and show no allocation per save/load. CRC equality supports observed correctness, not universal compatibility or cryptographic trust. Menu/gamepad/shader verification is separate actual frontend evidence, not something a headless oracle proves.
