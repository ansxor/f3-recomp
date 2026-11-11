# f3-recomp

Static recompilation of Taito F3 arcade games into native C, with a runtime built on SDL3.

## Building

**Requirements:** CMake 3.24+, Ninja, C11/C++20 compiler, Rust (cargo, rustc; edition 2024), Git, `uv` (managing Python 3.11+ and Capstone 5.0.9).

**macOS:**

```sh
brew install cmake ninja uv rust
# Optional: prebuilt libraries/tools, used instead of source builds when present.
brew install sdl3 yyjson cpp-httplib googletest glslang spirv-cross
```

C/C++ dependencies come from [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) (`cmake/dependencies.cmake`).
Installed packages (Homebrew, distro, Vulkan SDK) are linked as-is; anything missing is fetched at a
pinned tag and built from source, with sources cached in `~/.cache/CPM` across build directories.
Pass `-DCPM_DOWNLOAD_ALL=ON` to ignore installed packages.

The audio board and the EEPROM are Rust: `crates/f3rt-{audio,eeprom}` implement the C ABI declared in
`include/f3rt/abi/{audio,eeprom}.h`. They reach the C++ runtime through
[Corrosion](https://github.com/corrosion-rs/corrosion) (`cmake/corrosion.cmake`): `crates/f3rt-abi` is the
single staticlib linked into `f3rt`, and the other crates are its rlib dependencies. There are no external
crates (`Cargo.lock` lists only the workspace; the workspace builds with `cargo build -p f3rt-abi --release`;
`[profile.release]` in `Cargo.toml` is `opt-level = 3`, fat LTO, one codegen unit, `panic = "abort"`).

FAUST 2.x is only needed to regenerate the audio DSPs: the generated Rust under
`crates/f3rt-audio/src/hle/faust/generated/` is checked in, and `crates/f3rt-audio/faust/regen.sh` rebuilds it.

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
./build/landmakrj/landmakrj
```

Run with `--help` for all command-line options.

## Nintendo 3DS

```sh
nix run .#3ds-3dsx              # build build/landmakrj-3ds/landmakrj.3dsx
nix run .#3ds-cia               # build build/landmakrj-3ds/landmakrj.cia
nix run .#3ds-azahar            # build the 3dsx and launch it in Azahar
nix develop .#3ds               # devkitARM, makerom, bannertool, ctrtool, cmake, ninja
```

**Frame-budget bench (dev only):** `nix run .#3ds-bench` builds a separate 3dsx that replays a scripted
playthrough (attract, menu, character select, gameplay) to check frame timing. `tools/bench/n3ds-bench.sh`
runs it in Azahar and reports per-scenario results. It is never part of the shipped build.

**Real hardware:** audio needs `sdmc:/3ds/dspfirm.cdc`, a firmware dump taken from the console itself.
Without it the app runs silently and the bottom screen shows `Audio off: sdmc:/3ds/dspfirm.cdc missing`.
Azahar's HLE DSP ignores the file's contents, so a placeholder is enough there.

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

Architecture inspired by [N64Recomp](https://github.com/N64Recomp/N64Recomp). Video and sound algorithms adapted from BSD-3-Clause-licensed [MAME](https://github.com/mamedev/mame) sources. The main CPU runs only recompiled (static binary translation) code; there is no interpreter. Built with SDL3.

See [runtime/LICENSES.txt](runtime/LICENSES.txt) for full attribution.
