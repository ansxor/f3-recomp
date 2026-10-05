# Netplay oracle

Use the [netplay oracle guide](/developer/netplay/oracle) for binary modes/options and the [CLI reference](/reference/cli#f3rt-netplay-oracle) for flags. This page summarizes acceptance boundaries rather than duplicating the command catalog.

## What must agree

- Full local snapshot save/load/replay preserves same-geometry state, native pixels and accurate PCM.
- Canonical sync import works across differing presentation geometry while retaining native rendering/trails and hardware state. Unsafe parser fields are rejected, not merely size-checked.
- Real relay clients begin with independent genuine solo/EEPROM histories. Each versus match transfers a fresh host snapshot, crosses the both-loaded barrier, then compares with a delayed-input reference from that exact `handoff.bin` and match-index stream.
- Host authority works in either player slot; guest requested delay and presentation may differ. Natural exit is confirmed before return local, and rematches need no cold restart.
- Late input and recoverable stalls converge; mismatches reject, and peer departure restores confirmed state before local execution.

Compare canonical state CRC (`sync_state_crc`), native framebuffer CRC, confirmed PCM CRC and stereo sample count at the same confirmed boundary. Matching CRCs are observed evidence, not a portability/security guarantee. Full local CRCs with expanded buffers must not be substituted for canonical network CRCs.

## Commands

```sh
python3 tools/run_netplay_oracle.py --suite snapshot --sound-driver all
python3 tools/run_netplay_oracle.py --suite baseline --frames 20000 --seeds 1
python3 tools/run_netplay_oracle.py --suite impaired --frames 20000 --seeds 1 2 3 5 8 13 21 34
python3 tools/run_netplay_oracle.py --suite cases
build/f3rt-netplay-oracle --mode sync-proof --frames 2400 --seed 1 --sound-driver native
```

The runner logs lifecycle origins/handoffs, natural ends/local returns and per-match comparisons. Keep ROM-derived captures under ignored output directories. Headless checks do not verify menu capture, actual gamepads or postprocess surfaces; those require frontend exercises.

Current observed runs and frontend proof are recorded in [IMGUI-NETPLAY.md](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/IMGUI-NETPLAY.md). HLE uses separate non-rewound reconciliation and must not inherit accurate-PCM equivalence claims from the native oracle.
