# f3-recomp

Static recompilation of Taito F3 arcade games into native C, with an SDL3 runtime for video, sound, and input.

## Building

**Requirements:** CMake 3.24+, Ninja, C11/C++20 compiler, `uv` (managing Python 3.11+ and Capstone 5.0.9), SDL3, `glslangValidator`, `spirv-cross`. Google Highway is used if installed; otherwise CMake fetches it.

**macOS:**
```sh
brew install cmake ninja sdl3 glslang spirv-cross uv
```

**Build steps** (using your own legally obtained ROMs):

```sh
# Configure (example: Land Maker; uv manages Python dependencies automatically)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_ROM_DIR=/path/to/roms/landmakr

# Build
cmake --build build --target landmakr -j 4
```

For other games, set `-DF3_GAME=rayforce` (or `commandw`, `ridingf`) with a separate build directory. See [build options](docs/site/reference/build-options.md).

Or use the Make shortcuts, which wrap these commands: `make build GAME=rayforce`, `make test GAME=rayforce`, `make list-games`. Run `make help` for all targets. See [getting started](docs/site/guide/getting-started.md#make-shortcuts).

## Running

```sh
./build/landmakr --rom-dir /path/to/roms/landmakr --eeprom build/landmakr.nv
```

Run with `--help` for all command-line options, or see the [CLI reference](docs/site/reference/cli.md). Controls and input mapping are documented in the [input guide](docs/site/guide/input.md).

## Features

- **Video**: MAME-derived F3 renderer with optional GPU-accelerated presentation
- **Audio**: Accurate emulated sound devices (ES5505/ES5510); optional HLE mode

## Supported Games

The following ROM sets have tested native builds:

- **Land Maker** (Japan 2.01J, `landmakrj`) — Full native execution, enhanced GPU presentation
- **RayForce** (America 2.3A, `rayforce`) — Native execution, 224×320 display
- **Command War** (0.0J prototype, `commandw`) — Native execution (some graphics limitations)
- **Riding Fight** (World 1.0O, `ridingf`) — Native execution (missing upper planes/sprite trails)

## Documentation

- [Full documentation](https://ansxor.github.io/f3-recomp/) — Guides, reference, and developer docs
- [CLI reference](docs/site/reference/cli.md)
- [Video guide](docs/site/guide/video.md)
- [Sound guide](docs/site/guide/sound.md)
- [Developer evidence](docs/developer/README.md)

## Credits

Architecture inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp). Video and sound algorithms adapted from BSD-3-Clause-licensed [MAME](https://github.com/mamedev/mame) sources. CPU interpreter based on Musashi. Built with SDL3.

Taito game programs and assets remain property of their rights holders. No ROMs are included.

See [runtime/LICENSES.txt](runtime/LICENSES.txt) for full attribution.
