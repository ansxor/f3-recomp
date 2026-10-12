# f3-recomp

Static recompilation of Taito F3 arcade games into native C, with an SDL3 runtime for video, sound, and input.

## Building

**Requirements:** CMake 3.24+, Ninja, C11/C++20 compiler, Git, `uv` (managing Python 3.11+ and Capstone 5.0.9). Every C/C++ library and the shader tools (SDL3, Dear ImGui, Yojimbo, cpp-httplib, cpp-yyjson, Google Highway, zlib, GoogleTest, RapidCheck, `glslangValidator`, `spirv-cross`) come from the `vcpkg` submodule; the first configure bootstraps vcpkg and builds them.

**macOS:**
```sh
brew install cmake ninja uv
git submodule update --init vcpkg
```

**Build steps** (using your own legally obtained ROMs):

```sh
# Configure (example: Land Maker; uv manages Python dependencies automatically)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_ROM_DIR=/path/to/roms/landmakr

# Build
cmake --build build --target landmakr -j 4
```

For other games, set `-DF3_GAME=rayforce` (or `commandw`, `ridingf`) with a separate build directory. Build variables (`F3_GAME`, `F3_ROM_DIR`) are defined in `CMakeLists.txt`.

Or use the Make shortcuts, which wrap these commands: `make build GAME=rayforce`, `make test GAME=rayforce`, `make list-games`. Run `make help` for all targets.

## Running

```sh
./build/landmakr --rom-dir /path/to/roms/landmakr --eeprom build/landmakr.nv
```

Run with `--help` for all command-line options.

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

- `make help` lists build, test and run targets; each program's `--help` lists its options.
- [docs/notes/](docs/notes/index.md): per-routine, hardware, quirk and measurement notes for the supported games.
- [docs/SOUND-DRIVER.md](docs/SOUND-DRIVER.md): Land Maker sound-driver semantics and native/oracle comparison.

## Credits

Architecture inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp). Video and sound algorithms adapted from BSD-3-Clause-licensed [MAME](https://github.com/mamedev/mame) sources. CPU interpreter based on Musashi. Built with SDL3.

Taito game programs and assets remain property of their rights holders. No ROMs are included.

See [runtime/LICENSES.txt](runtime/LICENSES.txt) for full attribution.
