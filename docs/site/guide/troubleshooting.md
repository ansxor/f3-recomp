# Troubleshooting

This page lists errors, causes and corrective actions. Find the stage where the problem occurs. Then find the message.

The frontend prints `f3rt: MESSAGE` and exits with code 1. The recompiler prints `f3-recomp: MESSAGE` and exits with code 1. Other tools use different messages and codes. `f3rt-replay` prints the error without a prefix and exits with code 2. See [CLI reference](/reference/cli#general-rules).

```mermaid
flowchart TD
    A["Problem"] --> B{"When?"}
    B -->|"cmake configure"| C["Build stage: recompiler and sound compiler"]
    B -->|"cmake build"| D["Build stage: compiler and SDL"]
    B -->|"Program start"| E["Options and ROM loading"]
    B -->|"During the game"| F["Runtime errors"]
```

## Build stage: configure

The configure step runs the recompiler and the sound compiler. Both read your ROM files. If one fails, CMake stops with an error from `execute_process`. The real message is in the text above that error.

### Recompiler messages

| Message | Cause and action |
| --- | --- |
| `f3-recomp: Missing ROM lane file 'e61-13.20' in 'DIR' for game 'landmakrj'.` | The file is not in the ROM directory. Check the path in `F3_ROM_DIR` and the file name. The same message exists for `e61-12.19`, `e61-11.18` and `e61-10.17`. |
| `f3-recomp: ROM lane 'FILE' size mismatch: expected 524288 bytes, got N bytes.` | The file has a wrong size. Use the original dump. |
| `f3-recomp: ROM lane 'FILE' CRC32 mismatch: expected X, got Y.` | The file has a different content, or a different version of the game. Use the files in the table in [Getting started](/guide/getting-started#step-2-prepare-the-rom-files). |
| `f3-recomp: ROM lane 'FILE' SHA1 mismatch: expected X, got Y.` | Same cause as for CRC32. The tool checks both values. |
| An error that names a missing module, for example `No module named 'capstone'` | Dependencies are not synced in the virtual environment. Run `uv sync` from the repository root. |
| An error that names `uv` not found | `uv` is not installed or not in `PATH`. Install `uv` (e.g. `brew install uv` or via your package manager). |

The recompiler checks only the four program files. The other 11 ROM files are checked when you start the program. See [Program start](#program-start-rom-files).

The recompiler also has messages about the config file, for example `Config ... is missing [rom] section.`, `Unknown discovery coverage mode` and `Entry point must be word-aligned (even)`. They appear only if you edit `games/landmakrj/config.toml`. Restore the original file. (Code hooks were removed; `[[hooks]]` is no longer a valid key.)

### Sound compiler messages

| Message | Cause and action |
| --- | --- |
| `FileNotFoundError: Sound ROM chips e61-14.32 / e61-15.33 not found in DIR` | Add the two sound program files to the ROM directory. |
| `ValueError: Unexpected ROM chip sizes: A / B` | A sound file has a size other than 131072 or 262144 bytes. Use the original dump. |
| `ValueError: Sound ROM CRC mismatch: got 0x..., expected 0x5a7e9117` | The sound files do not match the Japanese set. |

### Other configure problems

| Symptom | Cause and action |
| --- | --- |
| CMake says it needs a newer version | The top-level `CMakeLists.txt` needs CMake 3.24 or newer. |
| CMake cannot find a package configuration file for `SDL3` | Install the SDL3 development files. If they are in a non-standard place, set `CMAKE_PREFIX_PATH` or `SDL3_DIR`. |
| You want no window program | Add `-DF3RT_SDL=OFF`. The `landmakr` program is then not built. |
| `Set F3_GENERATED_DIR to emitted program directory (sources.cmake missing)` | You used the `recomp` directory alone and gave no generated code. Use the top-level build with `F3_ROM_DIR`, or run `uv run python -m recomp emit` first. |

## Build stage: compile

| Symptom | Cause and action |
| --- | --- |
| The build takes very long and uses much disk space | Generated C is large. Use `-DCMAKE_BUILD_TYPE=Release` and reduce parallel jobs if memory is limited. |
| The compiler stops on warnings in `f3_recompiled` | The generated code is built with `-Wall -Wextra -Werror` on GCC and Clang. Report this as a bug with your compiler version. |

## Program start: options

These messages come from the option checks in `runtime/frontend/frontend.cpp`.

| Message | Cause and action |
| --- | --- |
| `Unknown argument: X` | The option name is wrong. Run `./build/landmakr --help`. |
| `Missing value for --X` | The option needs a value and it is last on the line. |
| `stoull: no conversion` or `stoul: no conversion` | A number option got text, for example `--frames abc`. Give a number. |
| `Headless execution requires --frames` | Add `--frames N` to `--headless`. |
| `--rom-dir required; --dump-every must be positive` | No ROM directory is known, or `--dump-every` is 0. |
| `This generated executable requires landmakrj` | `landmakr` accepts only `--set landmakrj`. |
| `--audio-backend must be enhanced or reference` | Use one of the two names. |
| `Enhanced audio requires landmakrj` | Enhanced audio exists only for `landmakrj`. Use `--audio-backend reference` for other sets. |
| `Enhanced audio does not execute a sound driver; --sound-trace requires reference audio` | Add `--audio-backend reference`, or drop `--sound-trace`. |
| `--renderer must be accurate, enhanced, game-cpu, compare-cpu or compare-gpu` | Use one of the five names (`accurate` and `enhanced` are the user-facing ones). |
| `Game-data video requires strict native execution` | You used `enhanced` or a developer game/compare renderer with `--allow-fallback`, `--set` other than `landmakrj`, or in `f3rt-run`. Use `landmakr` without `--allow-fallback`. |
| `--video-scale must be 1..4, auto or auto-integer` | Use a numeric scale 1–4 or one of the automatic GPU modes. |
| `--video-scale auto/auto-integer requires --renderer enhanced or compare-gpu` | Use `--renderer enhanced` or keep a fixed numeric scale. |
| `--video-border must be 0..160` | Use a value from 0 to 160. |
| `--video-filter must be nearest or linear` | Use one of the two names. |
| `Scale, border and filter options require --renderer enhanced (or a developer game/compare renderer)` | You gave scale, border or a filter other than `nearest` with `--renderer accurate`. Use `--renderer enhanced`. |
| `This binary was built without F3_GENERATED_DIR` | You used `--translated` with a program that has no generated code. |
| `Generated block registration failed` | The generated code does not match the runtime. Rebuild everything. |

### Program start: ROM files

The runtime loads 15 files. Each file is checked for size and CRC32. The paths in the messages are the paths that the runtime tried to open.

| Message | Cause and action |
| --- | --- |
| `Cannot open ROM: PATH` | The file is missing or not readable. Check the file name in the ROM directory. The recompiler did not check this file. |
| `Wrong ROM length: PATH` | The file has a different size from the table in [Getting started](/guide/getting-started#step-2-prepare-the-rom-files). |
| `ROM CRC mismatch: PATH` | The file content is different. Use the original dump. |
| `Padded sound ROM CRC mismatch: PATH` | A short sound file has a good CRC, but the padded result does not. Use the original dump. |
| `Cannot read ROM: PATH` | The file could not be read. Check permissions. |
| `Unsupported ROM set: X` | The set name is not `landmakrj` or `landmakr`. |
| `SoundNative: unsupported sound ROM CRC 0x... (expected 0x5a7e9117)` | The native sound driver supports only the Japanese sound ROM. |

### Program start: other files

| Message | Cause and action |
| --- | --- |
| `Invalid EEPROM file: PATH` | The `--eeprom` file does not have exactly 128 bytes. Delete it to start with a new EEPROM. |
| `EEPROM read failed` or `EEPROM write failed` | The program cannot read or write the file. Check the path and permissions. |
| `Cannot open sound trace: PATH` or `Sound trace write failed` | Check the `--sound-trace` path and the free disk space. |
| `WAV open failed` or `WAV write failed` | Check the `--wav` path and the free disk space. |
| `Fallback report write failed` | Check the `--fallback-report` path. |

### Program start: window and sound device

If SDL cannot start, the program prints the text that SDL gives. Typical causes: no display, or no sound device. To run without both, use `--headless --frames N`. To run with a window but without a sound device, use `--no-audio`.

## During the game

| Message or symptom | Cause and action |
| --- | --- |
| `Untranslated main CPU instruction at PC 0x...` | The generated code does not cover this address. The `landmakr` program stops by design. Please report it with the address. You can try `--allow-fallback` for a diagnostic run, but that run does not count as the native game. |
| `CPU halted at N` | The main CPU stopped. Please report the number. |
| `SoundNative: fatal unsupported reachable PC: 0x... (opcode 0x...)` | The native sound driver reached code that it does not cover. Please report it. Try `--audio-backend enhanced` (`landmakrj` only), which runs no sound driver. |
| `Game composite frame N: ... RGB pixel mismatches` | With `--renderer compare-cpu` or `compare-gpu` the two renderers disagree. See [Video and presentation](/guide/video#what-an-error-means-in-the-compare-renderers). |
| The game is slow | Check that you configured `-DCMAKE_BUILD_TYPE=Release`. Try `--renderer accurate` if GPU device or driver behavior is problematic.
| `f3rt: warning: GPU renderer unavailable (...); falling back to --renderer accurate` | The default or saved `enhanced` renderer could not start a GPU device or claim a window, so this session runs `accurate`. Fix the GPU driver, or set Renderer to `accurate` in F1 → Video. Passing `--renderer enhanced` explicitly makes this an error instead. | |
| No sound at the start | The game sets the output gain at about 13 seconds. Wait. Check that you did not give `--no-audio`. |
| Frontend settings or remaps are lost | Choose **Save preferences** in F1 and check the selected `--config` path. |
| Arcade game settings are lost | Add `--eeprom FILE`. See [Controls and options](/guide/running#settings-and-the-eeprom). |
| The keys do not work | Focus the window and close F1; game input is neutral with the menu open. Check bindings; P2 has only start/coin defaults, and gamepads require explicit bindings. |

## Ask for help

Open an issue at [github.com/ansxor/f3-recomp](https://github.com/ansxor/f3-recomp/issues). Include the full message, the command line, your operating system and your compiler version. Do not attach ROM files.
