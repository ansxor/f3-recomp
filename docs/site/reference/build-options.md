# Build options

## Requirements

CMake 3.24+, a C11/C++20 compiler and Python 3.11+ are required. Python generates runtime ROM manifest metadata on every configuration, even runtime-only builds. Capstone 5.0.9 (`recomp/requirements.txt`) is required only for CPU recompilation. SDL3 development files are required with `F3RT_SDL`; `glslangValidator` and `spirv-cross` are required when GPU/SDL support is enabled. Go is needed only for the separately built relay.

## Cache variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `F3_GAME` | `landmakrj` | Select `games/SET/config.toml`: `landmakrj`, `landmakr`, `rayforce`, `commandw`, `ridingf`. World LM remains config-only/unverified. |
| `F3_ROM_DIR` | empty | Selected title's chip directory; generates native main and sound programs and supplies title executable's default ROM path. |
| `F3_GENERATED_DIR` | empty | Pre-generated main C directory; ROM generation defaults to `BUILD_DIR/generated/SET`. |
| `F3_SOUND_GENERATED_DIR` | empty | Pre-generated sound C directory; ROM generation defaults to `BUILD_DIR/generated/sound-SET`. |
| `F3RT_SDL` | `ON` | Build SDL frontend/player targets. |
| `F3RT_GPU` | `ON` | GPU presentation and LM GPU regression harnesses; `OFF` retains CPU/FDP presentation without shader tools. |
| `F3_PROFILE_INSTRUMENT` | `OFF` | Allocation-free per-entry main/sound counters; requires ROM generation. Record with `--profile-out FILE`. |
| `F3_PROFILE_DEFAULT_TIERS` | `ON` | Select frozen full-coverage `profiles/landmakrj.profile` automatically only for Japan. |
| `F3_PROFILE_TIERS` | empty | Matching versioned full-coverage profile; hot units use `-O2`, cold use Clang `-Oz` or other compilers' `-Os`. |
| `F3_PROFILE_SLIM` | empty | Explicit code-removal experiment; cold hits abort, no fallback. Mutually exclusive with tiers; not recommended for general play. |
| `BUILD_TESTING` | `ON` | Runtime check targets and CTest registration; no ROMs needed for runtime checks. |

Keep independent build directories for each game/profile. Main/sound image guards reject mismatched generated code and loaded ROM CRC identities; pre-generated directories must match the selected set, not just contain similarly named files.

## Reproducible build and run

```sh
python3 -m pip install --target build/rayforce/python -r recomp/requirements.txt
cmake -S . -B build/rayforce -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_GAME=rayforce -DF3_ROM_DIR=/path/to/roms/rayforce -DF3RT_GPU=OFF
cmake --build build/rayforce --target rayforce -j 4
./build/rayforce/rayforce --headless --frames 3600
./build/rayforce/rayforce --frames 3600 --surface build/rayforce-surface.bmp
```

Replace `rayforce` throughout with `commandw` or `ridingf` for those titles. Both LM selections build the target/executable `landmakr`, not `landmakrj`; Japan is the default selection:

```sh
cmake -S . -B build/landmakrj -DF3_GAME=landmakrj \
  -DF3_ROM_DIR=/path/to/roms/landmakrj -DCMAKE_BUILD_TYPE=Release
cmake --build build/landmakrj --target landmakr
./build/landmakrj/landmakr
```

Title executables default to strict-native main and, when generated, native sound with accurate devices. New titles use FDP, not LM game-data/HLE/netplay or enhanced GPU/motion semantics. Evidence and revision limitations are in [porting](/developer/porting).

## Configuration modes and generated inputs

1. Set `F3_ROM_DIR` for automatic main/sound generation. CMake supplies `BUILD_DIR/python` through `PYTHONPATH` and uses the selected TOML for both compilers.
2. Set `F3_GENERATED_DIR` and optionally `F3_SOUND_GENERATED_DIR` for pre-generated code. Supply ROMs at run time; no built-in path exists without `F3_ROM_DIR`.
3. Leave generation variables empty for runtime/interpreter-only tools. Python metadata generation still occurs; Capstone is unnecessary.

Every configuration runs `tools/compile_roms.py` over game configs to write `rom_manifest.hpp`, using shared `recomp/roms.py` schema/validation. With ROM generation, CMake runs `python3 -m recomp emit --config games/SET/config.toml` and `tools/compile_sound.py --config games/SET/config.toml`, with matching ROM/output/profile arguments. Config/compiler/manifest inputs are tracked; failures stop configuration.

Region placement, absent planes, accepted LM short dumps and 256 KiB sound mirroring are in [game config](/reference/game-config). Profiles bind image identity/bounds; do not apply Japan's profile to another game. Exclusions remain fatal even when diagnostic interpretation is enabled. Full tiers retain cold code; slim does not.

## Targets and definitions

- `f3rt`: reusable runtime; `f3rt_musashi`, `f3rt_m68kmake`, `f3rt_musashi_generated`: interpreter/reference support.
- `f3_recompiled`: generated main library when `F3_GENERATED_DIR` is supplied; `f3_sound_recompiled`: generated sound library when its directory is supplied.
- `f3rt-run`: SDL diagnostic frontend; native main only when explicitly selected with linked generated code.
- `landmakr`, `rayforce`, `commandw`, `ridingf`: selected SDL title executable when main code is linked. Only one title target is selected per build.
- `f3rt-replay`, `f3rt-sound-extract`: runtime diagnostic tools; extraction packet semantics remain LM-specific, not a generic driver API.
- `f3rt-gameplay-regression`, `f3rt-netplay-oracle`: Japan-only generated-code campaigns.
- `f3rt-gpu` and Japan GPU/motion regression targets: GPU-enabled builds; shared availability does not imply enhanced presentation for new FDP titles.
- Runtime checks are registered when `BUILD_TESTING` is enabled.

`F3RT_GAME` marks the strict-native title frontend (replacing the old LM-specific frontend macro); `F3RT_DEFAULT_SET` supplies the selected set, and `F3RT_DEFAULT_ROM_DIR` supplies the configured path. `F3RT_GENERATED` enables generated main integration. `F3RT_SOUND_GENERATED` enables generated sound integration and defaults the frontend to native sound. `F3RT_GPU` enables GPU presentation code, still subject to Japan game-data eligibility.

Generated main C requires C11 and warning-clean compilation (`-Wall -Wextra -Werror` for Clang/GCC). Runtime builds use C++20 and `-Wall -Wextra -Wpedantic`. Standard CMake build type/compiler flags also affect build identity.

## Netplay build identity

`netplay_build.hpp` supplies a SHA-256 build identity covering compiler/platform/options, runtime/header/compiler/config inputs and generated main/sound code. Matching identity is required for Japan netplay, not proof of determinism or support for other games. Profiles' mode and optimization settings participate; generated content identifies retained addresses. The header changes only when its contents change. See [protocol](/developer/netplay/protocol).

## Checks and relay

```sh
ctest --test-dir build/rayforce --output-on-failure
(cd netplay/server && go build -o ../../build/netplay-server .)
```

Runtime CTest passed 4/4 during bring-up; ROM placement/corruption and existing codegen fixtures passed 14/14. These are finite evidence, not new-title audio parity or full campaign proof. See [tools](/reference/tools) for Python checks and LM-specific proof workflows.
