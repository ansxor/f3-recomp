# Tools catalog

This catalog covers `tools/` and related runtime and netplay programs. Each entry explains its purpose and gives a command example.

For all flags, see the [CLI reference](/reference/cli). For how the tests work inside, see [Testing](/developer/testing/).

All commands run from the repository root. The examples use `build/` as the build directory and `roms/landmakrj` as the ROM directory. Change these paths for your setup.

## Quick guide

| I want to ... | Use |
| --- | --- |
| Check that the game runs for a long time without a fallback | `tools/run_gameplay_regression.py` |
| Check that netplay gives the same result as one machine | `tools/run_netplay_oracle.py` |
| Compare my frames with MAME frames | `tools/mame/run_capture.sh`, then `f3rt-replay`, then `tools/compare_frames.py` |
| Compare my audio with MAME audio | `tools/compare_audio.py` |
| Compare the oracle and native sound drivers | `f3rt-gameplay-regression --sound-trace`, then `tools/compare_sound.py` |
| Test the CPU lowering against Musashi | `tools/differential/run.py` |
| Make the sound driver C code | `tools/compile_sound.py` |

## Programs built by CMake

| Program | Source | What it does |
| --- | --- | --- |
| `f3rt-gameplay-regression` | `tools/gameplay_regression.cpp` | Runs the native game for many frames with a seeded input schedule. It fails if the CPU halts or a fallback instruction runs. It can also compare the game-data renderer with the FDP renderer. |
| `f3rt-netplay-oracle` | `tools/netplay_oracle.cpp` | Full snapshot replay, canonical cross-presentation sync proof, handoff-loaded reference and real versus campaigns. |
| `f3rt-sound-extract` | `tools/sound_extract.cpp` | Boots the game, freezes the main CPU, injects sound packets, and records audio and a bus trace. |
| `f3rt-replay` | `runtime/replay.cpp` | Renders MAME captures with the f3rt video code, or renders a MAME audio trace to a WAV file. |
| `f3rt-check` | `runtime/check.cpp` | Unit-level self-test of the runtime devices. No arguments. Runs under CTest. |
| `netplay-server` | `netplay/server/*.go` | UDP relay for two players. Built with `go build`, not with CMake. |

```sh
# 40000 frames, 8 seeds
python3 tools/run_gameplay_regression.py --rom-dir roms/landmakrj

# One run with video comparison
./build/f3rt-gameplay-regression --seed 1 --frames 20000 --video-diff

# Inject one sound packet and save the result
./build/f3rt-sound-extract --sound-driver native --packet 038001 --seconds 3 --wav note.wav
```

## Python scripts in tools/

| Script | What it does | How to run it |
| --- | --- | --- |
| `run_gameplay_regression.py` | Runs `f3rt-gameplay-regression` for each seed (default seeds 1 to 8) and reports the failed seeds. | `python3 tools/run_gameplay_regression.py --rom-dir DIR` |
| `run_netplay_oracle.py` | Runs real relay campaigns from independent solo histories, compares each match to its host handoff reference, and checks natural return/rematch. Suites: snapshots, baseline, impaired (including transfer chunks), and cases (late input, stall, geometry/delay independence, host-P2, mismatch, disconnect). | `python3 tools/run_netplay_oracle.py --suite all` |
| `compile_sound.py` | Compiles the sound CPU ROM to C. CMake runs it for you. | `python3 tools/compile_sound.py --rom-dir DIR --output DIR` |
| `compare_frames.py` | Compares two frames, or two directories of frames. Reports mismatch count, maximum error, mean error, RMSE and PSNR. Can write diff images. It uses only the Python standard library. | `python3 tools/compare_frames.py REFERENCE ACTUAL --json` |
| `compare_audio.py` | Compares two WAV files after one fixed delay correction. Reports metrics for the whole range and for windows. It needs NumPy and SciPy. The metrics are evidence and not a pass or fail verdict. | `python3 tools/compare_audio.py ref.wav test.wav --json report.json` |
| `compare_sound.py` | Compares two sound bus traces record by record. Reports the first difference with the commands before it. | `python3 tools/compare_sound.py oracle.trace model.trace` |
| `decode_sound.py` | Decodes an F3SND2 sound trace to JSON lines (gzip if the name ends in `.gz`). Filters: `--notes-only`, `--commands-only`. | `python3 tools/decode_sound.py trace.bin --output events.jsonl.gz` |
| `test_discovery.py` | Unit tests for discovery, with a synthetic ROM. No game data. | See "Python unit tests" below. |
| `test_generate.py` | Unit tests that build generated blocks for synthetic code and run them across dispatch deadlines. Needs a C compiler. | See below. |
| `test_decode_sound.py` | Unit tests for `decode_sound.py`. | See below. |

### Python unit tests

The test files use `unittest`. They import `recomp` and `decode_sound`. Run them with both the repository root and `tools/` on the module path.

```sh
PYTHONPATH=.:tools python3 -m unittest discover -s tools -p "test_*.py"
```

Install the pinned Capstone dependency first. These are developer checks, not a replacement for exercising the player build.

## C++ and CMake helpers in tools/

| File | What it does |
| --- | --- |
| `gameplay_inputs.hpp` | Constants of the seeded input schedule (`f3rt::test::ScheduleConfig`): coin frame, start button pulses, and button mashing. `f3rt-gameplay-regression` and `f3rt-netplay-oracle` share it. |
| `netplay_build_id.cmake` | A CMake script that writes `netplay_build.hpp`, the build hash for the netplay handshake. CMake runs it at build time. See [Build options](/reference/build-options#generated-header-netplay-build-hpp). |

## tools/differential/

This package tests the recompiler lowering against the Musashi reference core. It generates random test cases, builds a C test runner, and runs each case on both implementations. It compares the registers and the status flags.

| File | What it does |
| --- | --- |
| `run.py` | Script wrapper. It adds the repository root to `sys.path` and calls `main()`. |
| `__main__.py` | Command-line entry point. Also available as `python3 -m tools.differential`. |
| `runner.py` | Builds the test runner with `CC` (default `clang`) and runs it. Contains `run_differential()`. |
| `generator.py` | Generates deterministic test cases, with a focus on boundary states such as sticky Z in ADDX, SUBX and NEGX, shift counts, and branch displacements. |
| `musashi_build.py` | Finds the Musashi source (`find_musashi_source`) and builds `libmusashi.a` with `CC` and `AR`. |
| `harness_abi.c`, `harness_abi.h` | C side of the test runner. It holds the test environment (`DiffEnv`) and the bridge to the Musashi core (for example `diff_musashi_setup`). |
| `export_cycles.py` | Writes `recomp/68020_cycles.csv` from a generated Musashi `m68kops.c`. |

```sh
python3 tools/differential/run.py --cases 2000 --seed 7
python3 tools/differential/run.py --filter add --cases 500 -v
```

## tools/mame/

These files compare f3rt with the MAME emulator. They record frames and audio from MAME and keep them for the replay tools. MAME is not part of this repository. The scripts contain default paths that are specific to the author's machine. Pass your own paths with the flags.

| File | What it does |
| --- | --- |
| `README.md` | Describes the capture folder layout, the capture protocol, and the MAME baseline. |
| `stage_roms.py` | Checks the CRC32 of the ROM files and writes `landmakrj.zip` for MAME. It pads the two short sound ROMs. |
| `run_capture.sh` | Stages the ROMs, then runs MAME with `capture.lua` on the attract mode. Writes `metadata.json`, one `frame_NNNN` directory for each frame, and a WAV file. |
| `capture.lua` | The MAME Lua script. It saves palette RAM, graphics RAM, the scroll registers, main RAM, the active sprite RAM, and the reference pixels for each chosen frame. It reads `F3_CAPTURE_DIR`, `F3_CAPTURE_START`, `F3_CAPTURE_COUNT`, `F3_CAPTURE_STEP` and `F3_CAPTURE_EXIT`. |
| `audio_trace.lua` | The MAME Lua script that records the sound CPU bus writes to a `F3AUD2` file. Set `F3_AUDIO_TRACE` to the output path. |

```sh
# Check ROMs and write the ZIP
python3 tools/mame/stage_roms.py --source roms/landmakr --board-source roms/puchicar

# Capture 10 frames (needs a MAME build)
tools/mame/run_capture.sh --mame /path/to/mame --start-frame 600 --count 10

# Render them with f3rt and compare
./build/f3rt-replay --rom-dir roms/landmakrj --captures captures/landmakrj_attract --output build/replay
python3 tools/compare_frames.py captures/landmakrj_attract build/replay
```

## Other programs

| Program | Source | Notes |
| --- | --- | --- |
| `python3 -m recomp discover` and `emit` | `recomp/` | The recompiler. See the [CLI reference](/reference/cli#python3-m-recomp). |
| `landmakr` and `f3rt-run` | `runtime/frontend.cpp` | The game frontends. See [Running the game](/guide/running). |

For the design of the test tools, read [Testing](/developer/testing/), [Differential tests](/developer/testing/differential) and [Gameplay regression](/developer/testing/gameplay-regression).
