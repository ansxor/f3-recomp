# f3-recomp

Static recompilation of Taito F3 arcade games into native C, with a runtime built on SDL3.

## Building

**Requirements:** CMake 3.24+, Ninja, C11/C++20 compiler, Git, `uv` (managing Python 3.11+ and Capstone 5.0.9).

**macOS:**
```sh
brew install cmake ninja uv
# Optional: prebuilt libraries/tools, used instead of source builds when present.
brew install sdl3 yyjson cpp-httplib googletest glslang spirv-cross
```

C/C++ dependencies come from [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) (`cmake/dependencies.cmake`).
Installed packages (Homebrew, distro, Vulkan SDK) are linked as-is; anything missing is fetched at a
pinned tag and built from source, with sources cached in `~/.cache/CPM` across build directories.
Pass `-DCPM_DOWNLOAD_ALL=ON` to ignore installed packages.

FAUST 2.x is not needed to build: the DSP headers under `runtime/audio/hle/faust/generated/` are
checked in, and without a FAUST compiler (the default) no build runs it. If one is configured
(`-DFAUST_EXECUTABLE=<path to faust>`, or `faust` on `PATH` when configuring), ordinary builds also
run it for any header older than its `.dsp`, `architecture.hpp` or `.lib` inputs, then recompile the
code that includes it. `cmake --build <build-dir> --target faust-regen` does the same on request and
leaves up-to-date headers alone; touch the `.dsp` (or delete the header) to force one.

**Build steps** (using your own legally obtained ROMs):

```sh
make build GAME=landmakrj ROM_DIR=roms/landmakrj
```

Also available:

```sh
make list-games
make help
```

## Running

```sh
./build-landmakrj/landmakr
```

Run with `--help` for all command-line options.

## Supported Games

The following ROM sets have tested native builds:

- **Land Maker** (Japan 2.01J, `landmakrj`) — Full native execution, enhanced GPU presentation
- **RayForce** (America 2.3A, `rayforce`) — Native execution, 224×320 display
- **Command War** (0.0J prototype, `commandw`) — Native execution (some graphics limitations)
- **Riding Fight** (World 1.0O, `ridingf`) — Native execution (missing upper planes/sprite trails)

## Documentation

- `make help` lists build, test and run targets; each program's `--help` lists its options.
- [docs/notes/](docs/notes/index.md): per-routine, hardware, quirk and measurement notes for the supported games.

## Credits

Architecture inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp). Video and sound algorithms adapted from BSD-3-Clause-licensed [MAME](https://github.com/mamedev/mame) sources. CPU interpreter based on Musashi. Built with SDL3.

See [runtime/LICENSES.txt](runtime/LICENSES.txt) for full attribution.
