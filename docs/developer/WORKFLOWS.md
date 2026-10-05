# Generation and verification workflows

Run these commands from the repository root. Normal play does not require
verification tooling. Keep ROMs, generated C, captures and extracted media out
of commits; use ignored `build/` for outputs. The supported native game path
must execute with zero main-CPU interpreter fallback.

## Native game build

The [README](../../README.md#build) is the canonical integrated build recipe.
`F3_ROM_DIR` generates Japanese main and sound CPU programs during configuration.
The current [CPU ABI](ABI-CHANGES.md) is version 3; regenerate both programs when
changing it. Full-coverage tiers retain cold dispatch entries; do not use the
experimental slim build as a general-play baseline.

For standalone main-program discovery or emission:

```sh
PYTHONPATH=build/python python3 -m recomp discover \
  --config games/landmakrj/config.toml \
  --rom-dir /path/to/roms/landmakr --output build/discovery
PYTHONPATH=build/python python3 -m recomp emit \
  --config games/landmakrj/config.toml \
  --rom-dir /path/to/roms/landmakr --output build/generated-main
```

The commands validate the program lanes, not the ROM directory's name. Emitted
`program.bin`, C shards, headers, coverage/lowering reports and `sources.cmake`
are ROM-derived outputs. See [generated files](../site/reference/generated-files.md)
and [per-game config](../site/reference/game-config.md) for their contracts.

## Finite native smoke and seeded gameplay

```sh
./build/landmakr --frames 3600 --headless --wav build/attract.wav
cmake --build build --target f3rt-gameplay-regression -j 4
python3 tools/run_gameplay_regression.py --rom-dir /path/to/roms/landmakr \
  --frames 40000 --seeds 1 2 3 4 5 6 7 8
```

The seeded harness cold-boots the real runtime, inserts coins, pulses start and
applies deterministic direction/button input. It rejects CPU fallback, halt and
execution errors. A successful seed is sampled behavior, not exhaustive game
coverage. `--video-diff` compares game-data layers and native RGB against the
MAME-derived FDP reference; unsupported sampled state is not counted as a match.
See [gameplay regression](../site/developer/testing/gameplay-regression.md).

## CPU and device checks

```sh
PYTHONPATH=build/python python3 -m unittest discover -s tools -p 'test_*.py'
PYTHONPATH=build/python python3 tools/differential/run.py \
  --musashi runtime/third_party/musashi \
  --output build/differential --cases 5000
cmake --build build --target f3rt-check -j 4
./build/f3rt-check
```

Instruction differential checks compare registers, PC/SR, elapsed reference-model
cycles, ordered bus writes and memory against independent Musashi stepping.
Unsupported lowerings must be reported separately, not counted as passing.
These checks do not establish physical-bus timing or integrated video/audio
accuracy. See [testing](../site/developer/testing/index.md).

## Sound, snapshots and reference captures

- [Sound-driver investigation](../SOUND-DRIVER.md): select `--sound-driver oracle` explicitly for interpreted captures; native ROM execution is not HLE.
- [Netplay design and oracle](../NETPLAY.md): save/load, deterministic replay and impaired-relay scenarios are separate checks.
- [MAME capture tooling](../../tools/mame/README.md): format-2 state/pixel alignment; format-1 bundles are not valid comparison pairs.
- [Measurement archive](README.md): original metrics and limits, rather than a new verification claim.

Trace replay, native-versus-oracle equivalence and captured MAME-output comparison
answer different questions. None proves physical TC0630FDP or audio-board
behavior. Preserve the comparison revision, ROM identity, input schedule and
host alongside any new measurements.
