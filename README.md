# f3-recomp

Static recompilation of Taito F3 game programs into native C, with an SDL3 runtime
for video, sound, input and optional rollback netplay. **Land Maker Japan 2.01J is
the supported target today**, not the whole F3 library.

## Game support

| Game / ROM set | Status |
| --- | --- |
| Land Maker Japan 2.01J (`landmakrj`) | Native main and sound CPU execution; attract mode, coin/start/controls and sampled gameplay exercised. Optional enhanced presentation and two-player netplay. |
| Land Maker World (`landmakr`) | ROM configuration only. No tested native build or gameplay support. |
| Other Taito F3 games | Not implemented. Shared F3 components are a starting point, not a compatibility claim. |

The executable is named `landmakr`, but runs **`landmakrj`**. A ROM directory's
name does not identify the revision: program chips are validated, and World ROMs
are not silently substituted. Coverage is finite; no exhaustive campaign/ending
or cross-platform compatibility claim. Runtime testing to date is on macOS
arm64, with Metal for GPU presentation.

Video implements **MAME-derived F3 rendering**, and the Land Maker game-data path
is compared against that renderer and captured MAME output. This is not a
chip-verified model of the TC0630FDP. Audio likewise uses MAME-derived ES5505,
ES5510 and board-device models; matching native and interpreted sound drivers
does not establish physical-board or exact MAME waveform equivalence.

## Build

Requirements: CMake 3.24+, Ninja, a C11/C++20 compiler, Python 3.11+, Capstone
5.0.9, SDL3 development files, and `glslangValidator` plus `spirv-cross` for the
GPU-capable build. On macOS:

```sh
brew install cmake ninja sdl3 glslang spirv-cross
```

From this checkout, using your own legally obtained Japanese ROM set:

```sh
python3 -m pip install --target build/python -r recomp/requirements.txt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_ROM_DIR=/path/to/roms/landmakr
cmake --build build --target landmakr -j 4
./build/landmakr
```

`F3_ROM_DIR` generates both CPU programs and becomes the executable's default
ROM path. Generated code and ROM-derived files stay under ignored `build/`.
Use `-DF3RT_GPU=OFF` to build without the shader tools; runtime presentation
still defaults to CPU even in a GPU-capable build. The default full-coverage
profile changes compile optimization tiers, not availability of cold code.
Experimental `F3_PROFILE_SLIM` removes code and is **not recommended for play**.
See [build options](docs/site/reference/build-options.md).

## Run and defaults

```sh
./build/landmakr --rom-dir /path/to/roms/landmakr --eeprom build/landmakr.nv
```

Controls: **arrows** move, **Z/X/C** buttons, **5/6** coin, **1/2** start,
**F1** service, **F2** test, **Escape** quit. **F11** or **Alt+Enter** toggles
fullscreen. Offline keyboard movement/buttons address P1; netplay maps them to
the assigned local player. Settings persist only when `--eeprom FILE` is supplied.

| Option | Default / purpose |
| --- | --- |
| `--rom-dir DIR` | ROM path configured at build time; override without rebuilding. |
| `--video game\|fdp\|compare` | `game` in strict-native `landmakr`; `fdp` selects the MAME-derived renderer, `compare` is a developer diagnostic. |
| `--video-backend cpu\|gpu` | `cpu`; GPU presentation is opt-in and requires game-data video. |
| `--video-scale 1..4\|auto\|auto-integer` | `1`; automatic window-pixel sizing requires GPU. |
| `--video-border 0..160` | `0` native columns per side; game-data presentation only. |
| `--video-filter nearest\|linear` | `nearest`; final presentation filtering. |
| `--video-interp off\|linear\|fit` | `off`; optional GPU line sampling above native scale. |
| `--video-interp-fields none\|geometry\|palette\|geometry,palette` | `geometry`; palette blending is separately opt-in. |
| `--sound-driver native\|oracle` | `native` for a ROM-generated build; `oracle` interprets the sound ROM. Both use emulated sound devices; native is not HLE. |
| `--eeprom FILE` | No persistence unless supplied. |
| `--frames N --headless` | Finite non-windowed run; headless requires a frame limit. |
| `--wav FILE`, `--no-audio` | Save PCM or disable live playback. |

The main CPU rejects untranslated instructions by default. `--allow-fallback`
is a diagnostic mode, not the supported native-game path. Unsupported
game-data video frames use the retained FDP renderer; that does not enable CPU
interpreter fallback. Enhanced presentation rerasterizes scene geometry using
original ROM artwork, not newly generated detail; native captures stay 320×232.

Example opt-in presentation:

```sh
./build/landmakr --video-backend gpu --video-scale auto-integer --video-border 48
```

`./build/landmakr --help` and the [CLI reference](docs/site/reference/cli.md)
cover all flags. See the [video guide](docs/site/guide/video.md) and
[sound guide](docs/site/guide/sound.md) for constraints.

## Netplay quickstart

Netplay is opt-in, two-player rollback with a UDP input relay. Use matching
builds and ROMs, a reachable relay, fixed scale **1** and border **0**. Go 1.22+
is required for the relay:

```sh
(cd netplay/server && go build -o ../../build/netplay-server .)
./build/netplay-server -addr 0.0.0.0:9000
```

On the two clients:

```sh
./build/landmakr --netplay-server SERVER:9000 --netplay-room example --netplay-player 1 --netplay-delay 2
./build/landmakr --netplay-server SERVER:9000 --netplay-room example --netplay-player 2 --netplay-delay 2
```

Insert a coin and press start on each client; use the game's normal challenge
flow to enter versus play. Keys address the assigned local player. Both peers
cold-boot factory EEPROM; persistence, CPU fallback, oracle sound and diagnostic
traces are rejected. Confirmed-only audio adds latency to avoid repeating
speculative sound. The relay is not encrypted or an anti-cheat service; use a
trusted network/server. See the [netplay guide](docs/site/guide/netplay.md).

## Land Maker-specific versus generic F3

The 68020 lifter, C CPU ABI, F3 memory/scheduling/device runtime and FDP renderer
are reusable components. ROM manifests, discovery metadata, exclusions and
execution profiles are per-game. The current build target, ROM loader, sound
compiler assumptions, producer hooks and game-data scene decoders still contain
Land Maker-specific choices. Another F3 game needs more than a new TOML file.

The [portability audit](docs/site/developer/porting.md) maps these boundaries,
lists bring-up requirements and records refactor candidates without changing
runtime behavior.

## Documentation and development

- [Published documentation](https://ansxor.github.io/f3-recomp/): guide, reference and developer sections.
- [Developer evidence](docs/developer/README.md): preserved measurements, validation scope and historical decisions.
- [Developer workflows](docs/developer/WORKFLOWS.md): generation and checks, separate from normal play.

The site builds without ROMs. With Node.js 22+:

```sh
cd docs/site
npm ci
npm run dev
# Production build and local preview:
npm run build
npm run preview
```

## Credits and licensing

Architecture inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp) and
[N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime). F3 video and
sound algorithms adapt individually BSD-3-Clause-licensed
[MAME](https://github.com/mamedev/mame) files; no MAME framework/GPL-only source
is linked into the runtime. Musashi supplies the interpreted CPU reference;
SDL3 supplies windowing, input and playback.

See [runtime/LICENSES.txt](runtime/LICENSES.txt) for upstream authors, pinned
sources and dependency terms, including Musashi's separate SoftFloat notice.
This checkout has no repository-wide license grant; component notices must not
be read as licensing all project code. Taito game programs, artwork and sound
remain the property of their rights holders. No ROMs or game assets are included.
