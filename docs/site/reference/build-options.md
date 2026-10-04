# Build options

This page lists CMake options, cache variables, targets and compile definitions. It also explains the netplay build identity.

**What you will learn:**

- Which cache variables you can set, and what each one does.
- Which targets CMake builds, and which libraries each target links.
- Which compile definitions change the behavior of a program.
- How the build makes the hash that netplay peers compare.

## Requirements

- CMake 3.24 or newer (top-level `CMakeLists.txt`).
- A C11 and C++20 compiler.
- SDL3 with CMake config files, only if `F3RT_SDL` is on.
- Python 3 with `capstone` 5.0.9, only if `F3_ROM_DIR` is set. The file `recomp/requirements.txt` pins the version.
- Go, only to build the relay server. CMake does not build the relay server.
- `glslangValidator`, `spirv-cross` and Python 3 for offline SPIR-V/MSL shader generation when `F3RT_GPU` and `F3RT_SDL` are on.

## Cache variables

Set a cache variable with `-DNAME=value` on the `cmake` command line.

| Variable | Type | Default | Meaning |
| --- | --- | --- | --- |
| `F3RT_SDL` | option (`ON`/`OFF`) | `ON` | Build the SDL3 frontends `f3rt-run` and `landmakr`. If `OFF`, CMake does not look for SDL3. |
| `F3RT_GPU` | option (`ON`/`OFF`) | `ON` | SDL3 GPU presentation and GPU regression harness; requires offline shader tools. `OFF` retains CPU presentation. |
| `F3_ROM_DIR` | path | empty | Directory with the Land Maker Japan ROM files. If set, CMake runs the recompiler and the sound compiler at configure time. |
| `F3_GENERATED_DIR` | path | empty | Directory with a generated program (`sources.cmake` and C files). If you set `F3_ROM_DIR` and leave this empty, CMake uses `BUILD_DIR/generated/landmakrj`. |
| `F3_SOUND_GENERATED_DIR` | path | empty | Directory with a generated sound program. If you set `F3_ROM_DIR` and leave this empty, CMake uses `BUILD_DIR/generated/sound-landmakrj`. |
| `F3_PROFILE_INSTRUMENT` | option | `OFF` | Compile allocation-free per-entry main/sound hit counters. Requires `F3_ROM_DIR` to regenerate both CPUs; `--profile-out FILE` enables recording with 30-second and exit flushes. |
| `F3_PROFILE_DEFAULT_TIERS` | option | `ON` | With `F3_ROM_DIR`, use the frozen `profiles/landmakrj.profile` full-coverage tiers unless an explicit tiers/slim profile is selected. `OFF` opts out without disabling config exclusions; clear a previously cached `F3_PROFILE_TIERS` override too. |
| `F3_PROFILE_TIERS` | filepath | empty cache; frozen profile selected automatically | Override the full-coverage partition: hot generated units `-O2`, cold units `-Oz` on Clang or `-Os` otherwise. Requires ROM generation and a matching versioned profile. |
| `F3_PROFILE_SLIM` | filepath | empty | Explicit opt-in removal of unprofiled main/sound code. Missing entries abort loudly with address, ROM CRC, re-profile hint and cold-hit record; no interpreter fallback. Mutually exclusive with tiers. |
| `BUILD_TESTING` | option | `ON` (from `include(CTest)`) | Build `f3rt-check` and register the CTest test. |

Other standard CMake variables, such as `CMAKE_BUILD_TYPE` and `CMAKE_C_FLAGS`, also change the build. The netplay build identity includes them. See below.

### Three ways to configure

1. **From ROMs (normal way).** Set `F3_ROM_DIR`. CMake generates the native program and the sound program during configure. Every target is available.
2. **From a generated directory.** Set `F3_GENERATED_DIR` (and optionally `F3_SOUND_GENERATED_DIR`) to directories that you made before. The ROM files are then needed only at run time. `landmakr` has no built-in ROM directory in this case, because `F3RT_DEFAULT_ROM_DIR` is empty.
3. **Interpreter only.** Set none of the variables. CMake builds the runtime library, `f3rt-replay`, `f3rt-sound-extract`, `f3rt-run` and `f3rt-check`. `f3rt-run` has no native code.

```sh
cmake -S . -B build -DF3_ROM_DIR=/path/to/roms/landmakrj -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Execution-profile builds

Use separate build directories for instrumentation, full-coverage tiers and slim.
Collect with both native CPUs; merge independent output paths with
`python3 tools/block_profile.py merge --output profiles/landmakrj.profile RUN.profile ...`.
Profiles contain identities, addresses and counts, never ROM bytes. The generated
inventories and `tools/block_profile.py report` distinguish executable entry
addresses from packed host functions. A gameplay profile is evidence of observed
execution, not proof that the remaining code can never execute.

The six `landmakrj` main/sound exclusions in `config.toml` apply before tier
assignment in every ROM-generated mode. Default tiers retain 336,775 main and
61,997 sound entries (30,146 / 7,519 hot); slim retains only the hot subset.
Hits or misses inside an excluded interval fail configuration with CPU/address/
range diagnostics. Changing compilation tiers does not relax ABI 3 exclusion
errors or permit fallback for excluded aliases.

```sh
# Full coverage with exclusions but ordinary Release optimization:
cmake -S . -B build/plain -DF3_ROM_DIR=/path/to/roms/landmakr \
  -DCMAKE_BUILD_TYPE=Release -DF3_PROFILE_DEFAULT_TIERS=OFF -DF3_PROFILE_TIERS=
# Explicit, non-default removal experiment:
cmake -S . -B build/slim -DF3_ROM_DIR=/path/to/roms/landmakr \
  -DCMAKE_BUILD_TYPE=Release -DF3_PROFILE_SLIM="$PWD/profiles/landmakrj.profile"
```

The frozen corpus includes seeded and user-confirmed partial campaign data,
not held-out gate seeds. Full tiers keep unobserved code; slim is rejected for
general play. Historical and combined measurements:
[BINSIZE-COMBINED.md](https://github.com/ansxor/f3-recomp/blob/main/docs/BINSIZE-COMBINED.md).


### What happens at configure time with F3_ROM_DIR

1. CMake makes `F3_ROM_DIR` an absolute path and finds Python 3.
2. CMake lists `recomp/*.py` and `recomp/*.csv`, and `games/landmakrj/config.toml`, as configure dependencies. If one of these files changes, the next build runs configure again.
3. CMake runs `python3 -m recomp emit --config games/landmakrj/config.toml --rom-dir F3_ROM_DIR --output F3_GENERATED_DIR` with the selected profile arguments. The environment variable `PYTHONPATH` starts with `BUILD_DIR/python`, so that a local `capstone` install is found.
4. CMake runs `python3 tools/compile_sound.py --config games/landmakrj/config.toml --rom-dir F3_ROM_DIR --output F3_SOUND_GENERATED_DIR` with the same profile arguments. Both generators apply the config exclusions. The sound compiler and selected profile are also configure dependencies.
5. If either command fails, the configure step fails (`COMMAND_ERROR_IS_FATAL ANY`).

The game config is fixed to `games/landmakrj/config.toml` in the top-level `CMakeLists.txt`.

## Targets

| Target | Kind | Created when | Sources | Links (PRIVATE unless noted) |
| --- | --- | --- | --- | --- |
| `f3rt_m68kmake` | executable | always | `runtime/third_party/musashi/m68kmake.c` | none |
| `f3rt_musashi_generated` | custom target | always | Runs `f3rt_m68kmake` on `m68k_in.c`. Writes `m68kops.c` and `m68kops.h` in `BUILD_DIR/musashi`. | depends on `f3rt_m68kmake` |
| `f3rt_musashi` | static library | always | Musashi `m68kcpu.c`, `softfloat/softfloat.c`, generated `m68kops.c`, `runtime/core_state.c` | none. Depends on `f3rt_musashi_generated`. |
| `f3rt` | static library | always | `rom.cpp`, `cpu_abi.cpp`, `machine.cpp`, `interpreter.cpp`, `video.cpp`, `game_video.cpp`, `game_tiles.cpp`, `game_text.cpp`, `game_sprites.cpp`, `game_lines.cpp`, `game_compositor.cpp`, `audio.cpp`, `sound_trace.cpp`, `sound_native.cpp`, `block_profile.cpp`, `netplay.cpp`, `netplay_transport.cpp` (all in `runtime/`), every `runtime/third_party/audio/*.cpp`, and `BUILD_DIR/netplay_build.hpp` | `f3rt_musashi` |
| `f3_sound_recompiled` | static library | `F3_SOUND_GENERATED_DIR` is set | `F3_SOUND_GENERATED_SOURCES` from the `sources.cmake` file of the sound output | `f3rt` (PUBLIC) |
| `f3_recompiled` | static library | `F3_GENERATED_DIR` is set (defined in `recomp/CMakeLists.txt`) | `F3_GENERATED_SOURCES` from the `sources.cmake` file of the program output | none |
| `f3rt-sound-extract` | executable | always | `tools/sound_extract.cpp` | `f3rt` |
| `f3rt-gameplay-regression` | executable | `F3_GENERATED_DIR` is set | `tools/gameplay_regression.cpp` | `f3rt`, `f3_recompiled` |
| `f3rt-netplay-oracle` | executable | `F3_GENERATED_DIR` is set | `tools/netplay_oracle.cpp` | `f3rt`, `f3_recompiled` |
| `f3rt-run` | executable | `F3RT_SDL` is on | `runtime/frontend.cpp` | `f3rt`, `SDL3::SDL3`, and `f3_recompiled` if `F3_GENERATED_DIR` is set |
| `landmakr` | executable | `F3RT_SDL` is on and `F3_GENERATED_DIR` is set | `runtime/frontend.cpp` | `f3rt`, `f3_recompiled`, `SDL3::SDL3` |
| `f3rt-gpu` | static library | `F3RT_SDL` and `F3RT_GPU` | SDL GPU backend, host-only interpolation analysis and generated `video_shaders.hpp` | `SDL3::SDL3` (PUBLIC) |
| `f3rt-gpu-regression` | executable | GPU support and generated main program | `tools/gpu_video_regression.cpp` | `f3rt`, `f3_recompiled`, `f3rt-gpu`; generated sound when available |
| `f3rt-replay` | executable | always | `runtime/replay.cpp` | `f3rt` |
| `f3rt-check` | executable | `BUILD_TESTING` is on | `runtime/check.cpp` | `f3rt` |

If `F3_SOUND_GENERATED_DIR` is set, CMake also links `f3_sound_recompiled` into these targets, when they exist: `f3rt-sound-extract`, `f3rt-gameplay-regression`, `f3rt-netplay-oracle`, `f3rt-run` and `landmakr`.

`f3rt-replay` and `f3rt-check` link neither generated library. The `f3rt-check` test does not need ROM files.

### Target properties

- **Include directories.** `f3rt` exports `include/` (PUBLIC) and uses `runtime/`, the repository root and the build directory privately. `f3rt_musashi` exports the Musashi directory.
- **Warnings.** `f3rt` builds with `-Wall -Wextra -Wpedantic`.
- **Generated C.** `f3_recompiled` builds with `-Wall -Wextra -Werror` on Clang and GCC. A warning in generated code stops the build. The library requires C11.
- **Language standards.** The project sets C11 and C++20 and requires the C++ standard.

### Musashi definitions

`f3rt_musashi` sets these private definitions. They select the 68000 and 68020 features that the interpreter needs.

| Definition | Value |
| --- | --- |
| `M68K_EMULATE_030` | `0` |
| `M68K_EMULATE_040` | `0` |
| `M68K_EMULATE_INT_ACK` | `1` |
| `M68K_EMULATE_TRACE` | `1` |
| `M68K_EMULATE_RESET` | `1` |
| `M68K_EMULATE_ADDRESS_ERROR` | `1` |

## Compile definitions

Source files use these definitions to turn code on or off.

| Definition | Set on | Value | Effect |
| --- | --- | --- | --- |
| `F3RT_GENERATED` | `f3rt-gameplay-regression`, `f3rt-netplay-oracle`, `f3rt-run` (if `F3_GENERATED_DIR`), `landmakr` | `1` | The program includes `program.h` and can call `f3_generated_register`. Without it, `--translated` fails at run time. |
| `F3RT_LANDMAKR` | `landmakr` | `1` | Changes the defaults of `runtime/frontend.cpp`: native execution on, fallback off, default ROM directory, and `landmakrj` only. See [CLI reference](/reference/cli#landmakr-and-f3rt-run). |
| `F3RT_SOUND_GENERATED` | Every target that links `f3_sound_recompiled` | `1` | The program includes `sound_program.h`. It can call `use_native_sound` with `f3_sound_blocks`. The frontend then defaults to the `native` sound driver. |
| `F3RT_GPU` | GPU-enabled frontends | `1` | Enables `--video-backend gpu`; headless and native outputs still use CPU. |
| `F3RT_DEFAULT_ROM_DIR` | `landmakr`; `f3rt-netplay-oracle` (always); `f3rt-gameplay-regression` (only if `F3_ROM_DIR` is set) | The string `F3_ROM_DIR` | Default value of the ROM directory. For `f3rt-netplay-oracle`, the string is empty if `F3_ROM_DIR` is empty. `f3rt-sound-extract` reads this definition too, but CMake does not set it for that target. |

## Generated header: netplay_build.hpp

The build writes `BUILD_DIR/netplay_build.hpp`. It has one line of content: `#define F3_NETPLAY_BUILD_HASH "<sha256>"`. The netplay code puts this hash in the handshake. Two players can only play together if their hashes are equal. The reason: the game is deterministic only if both players run the same code with the same compiler settings.

### Inputs of the hash

The hash is the SHA-256 of one string. The string contains these parts, in order:

1. **Platform and compiler identity.** CMake builds this string (`NETPLAY_IDENTITY`) from the system name, the processor, the pointer size, the C and C++ compiler IDs and versions, `CMAKE_BUILD_TYPE`, and the C and C++ flags. It also contains the flags of each build type (Debug, Release, RelWithDebInfo, MinSizeRel), `CMAKE_OSX_ARCHITECTURES`, `CMAKE_OSX_DEPLOYMENT_TARGET` and `CMAKE_SYSROOT`.
2. **Source files.** For each file, the script adds the relative path and the SHA-256 of the content. The files are sorted by path. They are:
   - `include/*.h`, `include/*.hpp` (recursive)
   - `runtime/*.c`, `*.h`, `*.cpp`, `*.hpp` (recursive, so this includes `runtime/third_party`)
   - `recomp/*.py`, `recomp/*.h`, `recomp/*.csv`
   - `games/*.toml`
   - `CMakeLists.txt`, `tools/compile_sound.py`, `tools/netplay_build_id.cmake`
3. **Generated files.** For `F3_GENERATED_DIR` and `F3_SOUND_GENERATED_DIR`, the script adds the SHA-256 of every `*.c` and `*.h` file, sorted by name, with the prefix `generated/`.

### When the hash changes

- A custom command runs `tools/netplay_build_id.cmake` in script mode (`cmake -P`) at build time. The command depends on all the files above. A change to a runtime file updates the hash without a new configure step and without a new run of the recompiler.
- The script does not write the header again if the content is equal. This avoids needless recompiling.
- The ROM content is not in the hash directly. But the generated C files come from the ROM, so a different ROM gives a different hash.
- Profile instrumentation, full/tier/slim mode and the selected cold optimization flag participate in the compiler/options identity. Profile paths do not; retained generated C content already identifies the selected addresses.

::: tip
Two machines with different compilers build different hashes. This is on purpose. Floating-point and compiler differences can break determinism. Players who want to play together must use the same build artifacts.
:::

For the handshake itself, read [Netplay protocol](/developer/netplay/protocol).

## Build and test commands

```sh
# Configure and build
cmake -S . -B build -DF3_ROM_DIR=/path/to/roms/landmakrj -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run the unit tests (f3rt-check)
ctest --test-dir build --output-on-failure

# Build only the player build
cmake --build build --target landmakr

# Build the relay server (not part of CMake)
(cd netplay/server && go build -o ../../build/netplay-server .)
```

For the Python tests in `tools/test_*.py`, see [Tools](/reference/tools). For the full build pipeline, read [Build pipeline](/developer/build-pipeline).
