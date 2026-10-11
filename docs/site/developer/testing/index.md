# Testing and verification strategy

This section explains how the project checks its code against references. You will learn which reference checks each component.
You will also learn how to run each check and interpret its limits.

## Why this project tests against oracles

The project compares game behavior with independent reference implementations.
The game ROM, MAME captures and instruction tests provide different kinds of evidence.
No single reference proves all hardware behavior.

An **oracle** is a reference implementation. The new code is the **candidate**.
A test runs both with the same input and compares their outputs.
A difference needs investigation. Either implementation can contain a fault.

This method is **differential testing**. It has two rules:

- The oracle must be independent. It must not share the code that the test checks.
- The comparison must be strict. The tools in this repository do not use fitted offsets, fuzzy time warps or hidden tolerances unless the tool says so.

::: info
Most gates in this section need the Land Maker ROM files. The ROMs are not in the repository. The repository has no automatic test run: the only workflow in `.github/workflows` builds this documentation site. You run every gate yourself.
:::

## Which oracle checks which component

Each check compares a specific component with a reference. Some components need more than one reference.

| Candidate (new code) | Oracle (trusted reference) | Compared data |
| --- | --- | --- |
| Lowered C for one 68EC020 instruction | Musashi 68EC020 core | D0-D7, A0-A7, PC, SR, cycles, bus writes, memory |
| Whole recompiled main program | MAME frames and RAM, and the Musashi interpreter | RGB pixels, main RAM, WAV output |
| FDP video renderer (`runtime/renderer/fdp/video.cpp`) | MAME frames | RGB pixels from captured video RAM |
| Game-data video (`--renderer game-cpu`) | The FDP renderer | Indexed layer pixels and final RGB |
| Recompiled sound driver (`--sound-driver native`) | Interpreted sound driver (`--sound-driver oracle`) | Every sound-bus record, then the WAV bytes |
| ES5505, ES5510, MB87078 device models | MAME audio write trace | PCM waveform metrics |
| Device models, scheduler, EEPROM | Hand-written expected values in `runtime/tests/*.cpp` | Cycle counts, IRQ order, register values |

The diagram shows the same relations. Arrows point from the oracle to the candidate it checks.

```mermaid
flowchart LR
    subgraph Oracles["Oracles"]
        MUS["Musashi 68EC020"]
        MAME["MAME frames, RAM and audio"]
        INT["Interpreted sound driver"]
        FDPO["FDP renderer"]
        HAND["Hand-written expectations"]
    end
    subgraph Candidates["Components under test"]
        EMIT["Emitter: lowered C per instruction"]
        MAIN["Recompiled main program"]
        FDP["FDP renderer"]
        GAME["Game-data video"]
        SND["Recompiled sound driver"]
        DEV["Audio device models"]
        MISC["Scheduler, EEPROM, DUART, mixer"]
    end
    MUS -->|"differential run"| EMIT
    MUS -->|"interpreter run"| MAIN
    MAME -->|"compare_frames, RAM"| MAIN
    MAME -->|"f3rt-replay"| FDP
    FDPO -->|"--video-diff"| GAME
    INT -->|"compare_sound"| SND
    MAME -->|"audio trace replay"| DEV
    HAND -->|"runtime-* tests"| MISC
```

Two oracles appear on both sides of the diagram. The FDP renderer is a candidate for MAME and an oracle for the game-data video. The Musashi core is the oracle for the emitter and also the engine of the interpreter. The repository vendors one copy of Musashi in `runtime/third_party/musashi` and keeps MAME-compatible fixes in it (see [differential testing](/developer/testing/differential)).

## The gates

A **gate** is one test that you run before you accept a change. The table lists every gate, what it proves, and what it does not prove.

| Gate | Proves | Does not prove |
| --- | --- | --- |
| [Differential harness](/developer/testing/differential) | Each lowered instruction gives the same registers, flags, cycles and bus writes as Musashi, for the generated cases. | Control flow between blocks. Discovery coverage. RESET and STOP. Device behavior. Correct timing of the whole machine. |
| [Python unit tests](/developer/testing/unit-checks) | Discovery rules and block deadline code work on small synthetic ROMs. The sound trace decoder rejects bad input. | Anything about the real ROM. |
| [`runtime-* tests`](/developer/testing/unit-checks) | Device models, scheduler, EEPROM, DUART, mixer and exception timing match hand-written expectations. | Whole-game behavior. |
| [Seeded gameplay regression](/developer/testing/gameplay-regression) | A cold boot plus a seeded input script runs for N frames with zero interpreter fallback, no CPU halt and no execution error. | That every game state works. Output correctness (it checks only that the run completes). |
| Gameplay regression with `--video-diff` | The game-data renderer equals the FDP renderer on sampled frames. | Equality with real hardware. Frames that fall back to the FDP renderer. |
| [MAME frame comparison](/developer/testing/frame-compare) | Native frames equal MAME frames on the sampled frames. Main RAM equals MAME RAM at frame 600. | Frames between samples. RAM equality after frame 600. |
| [`f3rt-replay` video mode](/developer/testing/frame-compare) | The FDP renderer draws the same picture as MAME from the same video RAM. | CPU behavior. |
| [Audio trace replay](/developer/testing/audio-compare) | The sound device models produce a waveform that correlates with MAME for the same writes. | Exact waveform equality. Game-level sound timing. |
| [Sound trace comparison](/developer/testing/sound-tools) | The recompiled sound driver issues exactly the same bus records, at the same times, as the interpreted driver. | Equality with MAME. |

## Which gate to run for a change

Use this table to choose gates. Run every gate in the row.

| You changed | Run these gates |
| --- | --- |
| `recomp/emitter.py`, `recomp/cpu_ops.h`, `recomp/bitfield.h` | Differential harness, Python unit tests, gameplay regression, MAME frame comparison |
| `recomp/timing.py`, `recomp/*_cycles.csv` | Differential harness (it compares cycles), `runtime-* tests`, MAME frame comparison |
| `recomp/discovery.py`, `recomp/generate.py`, `games/*/config.toml` | Python unit tests, gameplay regression (it rejects any fallback), MAME frame comparison |
| `runtime/machine.cpp`, `runtime/cpu_abi.cpp`, `runtime/interpreter.cpp` | `runtime-* tests`, gameplay regression, MAME frame comparison, sound trace comparison |
| `runtime/renderer/fdp/video.cpp` | `f3rt-replay` video mode, MAME frame comparison |
| `runtime/renderer/game/*.cpp` | `runtime-* tests`, gameplay regression with `--video-diff`, MAME frame comparison |
| `runtime/audio/audio.cpp`, `runtime/third_party/audio/*` | `runtime-* tests`, audio trace replay, sound trace comparison |
| `tools/compile_sound.py`, `runtime/audio/reference/native/sound_native.cpp` | Sound trace comparison and WAV comparison |
| `runtime/state_io.*`, snapshot code | `runtime-* tests` (the `state` area), gameplay regression |

## Which gates need ROMs

| Gate | Needs ROM files | Needs MAME binary | Needs other tools |
| --- | --- | --- | --- |
| Differential harness | No | No | `uv`, a C compiler |
| Python unit tests | No | No | `uv`, a C compiler |
| `runtime-* tests` / `ctest` | No | No | CMake build with `BUILD_TESTING` on |
| Gameplay regression | Yes | No | Build with `F3_ROM_DIR` |
| Sound extraction (`f3rt-tool`) | Yes | No | Build with `F3_ROM_DIR` |
| Sound trace comparison | Yes (to make traces) | No | `uv` |
| MAME capture | Yes | Yes | `uv`, the staged ROM zip |
| Frame comparison | The captures come from ROMs | Only to make the reference | `uv` |
| Audio comparison | The WAV files come from ROMs | Only to make the reference | `uv` (analysis group: NumPy, SciPy) |
| `f3rt-replay` | Yes | Captures come from MAME | Build of `f3rt-replay` |

The three gates in the first three rows can run without any game data. Use them first. They are the only gates that a contributor without ROMs can run.

## How to run each gate

All commands run from the repository root. Pages in this section explain each command in detail.

### Gates without ROMs

```sh
# Python unit tests
uv run pytest

# Instruction-level differential harness
uv run f3 differential --cases 5000

# Device and scheduler checks
cmake -S . -B build -DF3RT_SDL=OFF
ctest --test-dir build -R runtime-
```

### Gates with ROMs

Configure the build with the ROM directory. The build then generates the recompiled main program and sound driver.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR=/path/to/roms/landmakr
cmake --build build --target f3rt-tool -j 4

# Seeded gameplay regression, eight seeds
uv run f3 gameplay-seeds --rom-dir /path/to/roms/landmakr \
  --frames 40000 --seeds 1 2 3 4 5 6 7 8

# Same run with the game-data renderer compared with the FDP oracle
uv run f3 gameplay-seeds --rom-dir /path/to/roms/landmakr \
  --frames 6000 --seeds 5 --video-diff
```

### Gates with MAME

```sh
uv run python tools/mame/stage_roms.py
./tools/mame/run_capture.sh --start-frame 600 --count 25 --step 120
uv run python tools/compare_frames.py captures/landmakrj_attract/ build/captures/native/
```

See [MAME oracle and captures](/developer/testing/mame) and [frame comparison](/developer/testing/frame-compare) for the full steps.

## How the gates build on each other

Start with isolated checks. Then run the relevant whole-machine checks.
If a whole-machine check fails, use the isolated checks to narrow the cause.

```mermaid
flowchart TB
    A["1. Unit checks: runtime-* tests, unittest"]
    B["2. Differential harness: one instruction vs Musashi"]
    C["3. Gameplay regression: strict native, no fallback"]
    D["4. MAME frames and RAM"]
    E["5. Video-diff and sound-trace compare"]
    A --> B --> C --> D --> E
```

Instruction tests identify errors in individual lowerings. Gameplay tests identify missing code paths and execution failures.
MAME comparisons expose output differences, including timing errors.

## Evidence in the repository

Historical measurements live separately from the tool documentation.

The [evidence index](/developer/evidence) links canonical validation, decisions,
and subsystem records. Those documents retain measured results and their scenarios.
This section describes the tools and comparison boundaries rather than duplicating
their measurement logs.

## Pages in this section

- [Differential instruction harness](/developer/testing/differential)
- [Seeded gameplay regression](/developer/testing/gameplay-regression)
- [MAME oracle and captures](/developer/testing/mame)
- [Frame comparison](/developer/testing/frame-compare)
- [Audio comparison](/developer/testing/audio-compare)
- [Sound traces and sound tools](/developer/testing/sound-tools)
- [Unit checks](/developer/testing/unit-checks)
