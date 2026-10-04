# Getting started

Install the tools and prepare your ROM files. Then build the game and start `landmakr`. This page gives each command.

::: warning You need your own ROM files
The repository contains no ROM data. You must supply a legally obtained Land Maker ROM set. The project cannot help you find ROM files.
:::

## Prerequisites

Install these tools before you build.

| Tool | Version | Used for |
| --- | --- | --- |
| CMake | 3.24 or newer | Configure and build. The top-level `CMakeLists.txt` requires 3.24. |
| Ninja | any recent version | Build backend. The commands below use `-G Ninja`. |
| C and C++ compiler | C11 and C++20 | Compile the runtime and the generated C. Use Clang or GCC. |
| SDL3 | development files | Window, keyboard and audio. CMake runs `find_package(SDL3 CONFIG REQUIRED)`. |
| Python | 3.11 or newer | Run the recompiler. It reads TOML with the standard `tomllib` module. |
| Capstone | exactly 5.0.9 | Decode 68k instructions. It is pinned in `recomp/requirements.txt`. |
| Go | 1.22 or newer | Build the relay server. Only needed for [online play](/guide/netplay). |

The netplay client uses POSIX sockets. The authors tested on macOS (Apple silicon). Other systems are untested.

## Step 1: Get the source

Clone the repository.

```sh
git clone https://github.com/ansxor/f3-recomp.git
cd f3-recomp
```

## Step 2: Prepare the ROM files

The runtime loads 15 files from one directory. Put all of them in the same directory. The names must match exactly. The runtime checks the size and the CRC32 of each file.

The recompiler uses only the four program files. The runtime and the sound compiler use the other files.

| File | Content | Size in bytes | CRC32 |
| --- | --- | ---: | --- |
| `e61-13.20` | Main program, byte lane 0 | 524288 | `0af756a2` |
| `e61-12.19` | Main program, byte lane 1 | 524288 | `636b3df9` |
| `e61-11.18` | Main program, byte lane 2 | 524288 | `279a0ee4` |
| `e61-10.17` | Main program, byte lane 3 | 524288 | `daabf2b2` |
| `e61-03.12` | Sprite graphics | 2097152 | `e8abfc46` |
| `e61-02.08` | Sprite graphics | 2097152 | `1dc4a164` |
| `e61-01.04` | Sprite graphics, high planes | 2097152 | `6cdd8311` |
| `e61-09.47` | Tile graphics | 2097152 | `6ba29987` |
| `e61-08.45` | Tile graphics | 2097152 | `76c98e14` |
| `e61-07.43` | Tile graphics, high planes | 2097152 | `4a57965d` |
| `e61-14.32` | Sound program | 262144 (see below) | `18961bbb` |
| `e61-15.33` | Sound program | 262144 (see below) | `2c64557a` |
| `e61-04.38` | Sound samples | 2097152 | `c27aec0c` |
| `e61-05.39` | Sound samples | 2097152 | `83920d9d` |
| `e61-06.40` | Sound samples | 2097152 | `2e717bfe` |

The sound program files have two accepted forms. The runtime accepts a 262144-byte file with the CRC32 in the table. It also accepts a 131072-byte file. The runtime pads that file with `0xff` bytes to 262144 bytes. The short CRC32 values are `b905f4a7` for `e61-14.32` and `87909869` for `e61-15.33`. The sound compiler also pads a 131072-byte file. It then checks the CRC32 of the combined sound ROM. That value is `5a7e9117`.

The four program files for the Japanese set have the names `e61-10` to `e61-13`. The World set uses different program files (`e61-16` to `e61-19`). Do not use World program files. The `landmakr` program supports only `landmakrj`.

The build also checks the SHA-1 of each program file against `games/landmakrj/config.toml`. A wrong or damaged file stops the build with a clear message. See [Troubleshooting](/guide/troubleshooting).

## Step 3: Install Capstone

Install the pinned Python package into the `build/python` directory. CMake adds this directory to `PYTHONPATH` when it runs the recompiler.

```sh
python3 -m pip install --target build/python -r recomp/requirements.txt
```

Use `python3` version 3.11 or newer. Older versions do not have `tomllib`.

## Step 4: Configure and build

Run this command in the repository root. Replace `../roms/landmakr` with the path of your ROM directory.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR=../roms/landmakr &&
cmake --build build --target landmakr -j 4
```

Run the command from the repository root. A relative `F3_ROM_DIR` is relative to that directory. The program stores the ROM directory as its default.

The Japan config exclusions and full-coverage compile tiers are on by default.
Hot code uses `-O2`; cold code uses Clang `-Oz` (or `-Os` on other supported
compilers). Every nonexcluded entry remains native. To keep exclusions but
disable automatic tiers, add `-DF3_PROFILE_DEFAULT_TIERS=OFF` (and
`-DF3_PROFILE_TIERS=` if reusing an explicit profile override).
An explicit `-DF3_PROFILE_TIERS=/path/to/profile` replaces the frozen corpus.
`F3_PROFILE_SLIM` removes cold code and is not the playable default.


::: tip Use a Release build
Use the `Release` build type. The authors measured only Release builds. Netplay also compares the build type between the two players.
:::

### What the build does

The configure step does the recompilation. The build step compiles the result.

```mermaid
flowchart TD
    A["cmake configure with F3_ROM_DIR"] --> B["python -m recomp emit"]
    A --> C["python tools/compile_sound.py"]
    B --> D["build/generated/landmakrj: C code for main CPU"]
    C --> E["build/generated/sound-landmakrj: C code for sound CPU"]
    D --> F["cmake --build --target landmakr"]
    E --> F
    G["runtime sources and vendored Musashi"] --> F
    F --> H["build/landmakr"]
```

1. CMake runs `python3 -m recomp emit`. The tool verifies the four program files, finds all instructions and writes C into `build/generated/landmakrj`.
2. CMake runs `python3 tools/compile_sound.py`. The tool verifies the sound ROM and writes C into `build/generated/sound-landmakrj`.
3. Ninja compiles the runtime library `f3rt`, the generated code and the frontend. It links them into `build/landmakr`.

Generated C remains large: the measured combined build contains 175,404,686 bytes across main and sound sources. Its arm64 executable is 31,525,032 bytes. These are one Release build's measurements, not cross-platform size guarantees. Change `-j 4` to match your CPU.

The generated directories are in `build/`, which Git ignores. Do not commit them.

## Step 5: Start the game

Run the program from the repository root.

```sh
./build/landmakr
```

The program opens a window and starts the game. The game uses the ROM directory that you gave to CMake. To use another ROM directory, add `--rom-dir`:

```sh
./build/landmakr --rom-dir /path/to/other/landmakr
```

Press `5` to insert a coin. Press `1` to start. The next page lists all keys: [Controls and options](/guide/running).

::: info The first seconds are quiet
At the start the game shows its own boot sequence. In a 700-frame test run in the author's build, the audio peak was 0. The README of the repository says that the game sets its output gain at about 13.23 seconds. This is normal.
:::

## Check that the build works without a window

You can test the build without a display. This command runs 300 frames and prints one summary line.

```sh
./build/landmakr --headless --frames 300 --no-audio
```

The last line starts with `set=landmakrj frames=300`. It also shows `fallback_instructions=0`. This value means that the generated code ran every instruction. A larger value means the game used the slow diagnostic interpreter. Read [Controls and options](/guide/running#the-summary-line) for all fields.

## Other build targets

The build creates more programs. You do not need them to play.

| Target | Purpose |
| --- | --- |
| `landmakr` | The game. Strict native code, native sound, game-data video. |
| `f3rt-run` | A diagnostic frontend. It can interpret the main CPU instead of using generated code. |
| `f3rt-replay` | Replays MAME capture data through the renderer and the audio chips. Used for tests. |
| `f3rt-gameplay-regression` | Headless seeded gameplay test. |
| `f3rt-netplay-oracle` | Netplay reference and snapshot test. |
| `f3rt-sound-extract` | Plays sound commands and writes WAV and trace files. See [Sound](/guide/sound). |
| `f3rt-check` | Unit checks for the devices. CTest runs it. |

Build one with `cmake --build build --target NAME`. For all CMake options see the [build options reference](/reference/build-options).

## Next steps

- Learn the keys and options: [Controls and options](/guide/running).
- Change picture size and filter: [Video and presentation](/guide/video).
- Play online: [Online play](/guide/netplay).
