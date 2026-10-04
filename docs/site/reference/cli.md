# Command-line reference

This page lists the project commands and their arguments. The tables give defaults, value limits and error behavior.

**What you will learn:**

- Which flags `landmakr` and `f3rt-run` accept, and how the two programs differ.
- Which flags the test and analysis programs accept.
- Which flags the Python tools and the netplay relay server accept.

## General rules

- The C++ parsers use separate arguments: `--flag value`, not `--flag=value`.
- Frontend options with missing values report `Missing value for ...`. Unknown frontend options report `Unknown argument: ...`.
- Other programs have their own error messages. For example, `f3rt-replay` reports usage for unknown options and missing values.
- C++ numeric options use conversions such as `stoull`. Invalid text raises a conversion error. These conversions can accept a numeric prefix with trailing text.
- The ROM directory must contain the Land Maker ROM files. See [Game config](/reference/game-config) for the file names.
- Build targets and their default values come from CMake. See [Build options](/reference/build-options).

| Program | Source | Exit code on error |
| --- | --- | --- |
| `landmakr`, `f3rt-run` | `runtime/frontend.cpp` | 1 (message starts with `f3rt:`) |
| `f3rt-replay` | `runtime/replay.cpp` | 2 |
| `f3rt-gameplay-regression` | `tools/gameplay_regression.cpp` | 1 (message starts with `REGRESSION ERROR:`) |
| `f3rt-sound-extract` | `tools/sound_extract.cpp` | 1 (message starts with `SOUND_EXTRACT ERROR:`) |
| `f3rt-netplay-oracle` | `tools/netplay_oracle.cpp` | 1 (message starts with `ORACLE ERROR:`) |
| `f3rt-check` | `runtime/check.cpp` | 1 (message starts with `FAIL`) |
| `f3rt-gpu-regression` | `tools/gpu_video_regression.cpp` | 1 (`GPU REGRESSION ERROR:`) |

## landmakr and f3rt-run

Both programs use `runtime/frontend.cpp`. `f3rt-run` is the general frontend. `landmakr` is the player build, available with `F3_GENERATED_DIR`. Its `F3RT_LANDMAKR` definition changes the defaults.

### Differences between the two builds

| Item | `landmakr` (`F3RT_LANDMAKR`) | `f3rt-run` |
| --- | --- | --- |
| `--rom-dir` default | The value of `F3_ROM_DIR` at configure time (`F3RT_DEFAULT_ROM_DIR`) | None. The flag is required. |
| Native execution (`--translated`) | On by default | Off. Pass `--translated` to use the recompiled code. |
| `--allow-fallback` default | Off (strict native) | On (the CPU may use the interpreter for untranslated code) |
| `--set` | Must be `landmakrj`. Other values stop with `This generated executable requires landmakrj`. | `landmakrj` or `landmakr` |
| `--video` default | `game` when strict native and you did not pass `--video` | `fdp` |
| `--sound-driver` default | `native` if the build has the generated sound program, else `oracle` | The same rule. |
| Linked generated program | Always (`F3RT_GENERATED`) | Only when `F3_GENERATED_DIR` is set |

::: info
The `--help` text says that the sound driver default is "native in landmakr; oracle in f3rt-run". The code is different. The default is `native` in every build that defines `F3RT_SOUND_GENERATED`. The code in `runtime/frontend.cpp` is the source of truth.
:::

### What changes with --allow-fallback

The fallback is the 68020 interpreter. The interpreter runs instructions that the recompiler did not translate. It is only for diagnosis.

| Effect of `--allow-fallback` | Detail |
| --- | --- |
| Machine setting | Sets `Machine::allow_main_fallback` to true. Without it, an untranslated instruction stops the program with `Untranslated main CPU instruction at PC ...`. |
| Default video in `landmakr` | The default stays `fdp`. The game-data renderer needs the strict native path. |
| `--video game` or `--video compare` | Rejected with `Game-data video requires strict native landmakrj`. |
| Netplay | Rejected. See the netplay rules below. |
| Counting | The program counts fallback instructions. `--fallback-report` writes the count for each address. |

### Flags

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | `landmakr`: `F3RT_DEFAULT_ROM_DIR`. `f3rt-run`: none | Directory that holds the ROM files | An omitted or empty directory fails validation: `--rom-dir required; --dump-every must be positive`. A missing argument reports `Missing value for --rom-dir`. |
| `--set` | `landmakrj` or `landmakr` | `landmakrj` | ROM set to load | Other values stop with `Unsupported ROM set`. The ROM loader checks the CRC32 of every file. |
| `--frames` | `N` | `0` | Stop after N video frames | `0` means no limit. A window run stops when you close the window or press Escape. |
| `--headless` | none | off | Do not open a window and do not open audio | Requires `--frames`. Without it: `Headless execution requires --frames`. |
| `--no-audio` | none | audio on | Do not open the audio device | The audio is still generated. `--wav` still receives the samples. |
| `--translated` | none | `landmakr`: on. `f3rt-run`: off | Run the recompiled native code | Needs a build with a generated program. Else: `This binary was built without F3_GENERATED_DIR`. |
| `--allow-fallback` | none | `landmakr`: off. `f3rt-run`: on | Let the interpreter run untranslated instructions | Diagnostic only. See the table above. |
| `--unthrottled` | none | throttled | Run as fast as possible | Without it, the program waits so that the game runs at the real frame rate. In netplay, it also removes the frame pacing and sets the lead limit to 16 frames. |
| `--eeprom` | `FILE` | none | EEPROM file | The program loads the file at start if it exists, and writes it at exit. The file must be exactly 128 bytes (else `Invalid EEPROM file`). Netplay rejects this flag. |
| `--wav` | `FILE` | none | Write the generated audio to a 16-bit stereo WAV file | The sample rate is the rate of the audio core. |
| `--sound-trace` | `FILE` | none | Write a bus trace of the sound CPU in the F3SND2 format | Netplay rejects this flag. Decode the trace with `tools/decode_sound.py`. |
| `--sound-driver` | `oracle` or `native` | See the table above | Choose the sound CPU implementation | `oracle` runs the interpreted 68000 sound driver. `native` runs the recompiled driver. `native` needs a generated sound program (`F3_ROM_DIR`). Else: `Native sound requires a generated sound program (F3_ROM_DIR)`. |
| `--profile-out` | `FILE` | none | Merge versioned main/sound entry counts, atomically flush every 30 seconds and on exit | Requires `F3_PROFILE_INSTRUMENT=ON` and strict native main/sound execution. Relative destinations are fixed against the startup working directory, including after later cwd changes. Slim builds also accept it for immediate `miss` records; cold aborts always append a durable `.cold-hits` log (or `f3-cold-hits.log` without this flag). Concurrent writers must use separate paths and merge later. |
| `--fallback-report` | `TSV` | none | Write a tab-separated list of `pc` and `count` for every instruction that used the fallback | Netplay rejects this flag. |
| `--dump-dir` | `DIR` | none | Write the machine state to `DIR/frame_NNNN/` | Files: `palette.bin`, `graphics.bin`, `control.bin`, `mainram.bin`, `shared.bin`, `rendered.argb`, `rendered.bmp`, `cpu.json`. |
| `--dump-start` | `N` | `1` | First frame to dump | |
| `--dump-every` | `N` | `1` | Dump every N frames | `0` is an error. |
| `--surface` | `BMP` or GPU `PNG` | none | Save the CPU window surface or GPU internal-resolution image at `--frames` | Window runs only. Native dumps remain CPU-produced. |
| `--video` | `fdp`, `game` or `compare` | `fdp`. In `landmakr` strict native: `game` | Choose the video renderer | `fdp` draws from the F3 video chip memory. `game` draws from the game data. `compare` runs both and checks them. `game` and `compare` need strict native `landmakrj`. |
| `--video-scale` | `1` to `4` | `1` | Integer scale of the internal picture | Else: `--video-scale must be 1..4`. Needs `--video game` or `compare`. |
| `--video-border` | `0` to `160` | `0` | Extra scene columns on each side of the 320-column picture | Else: `--video-border must be 0..160`. Needs `--video game` or `compare`. |
| `--video-filter` | `nearest` or `linear` | `nearest` | Texture filter for the window | Needs `--video game` or `compare` if you choose `linear`. |
| `--video-backend` | `cpu` or `gpu` | `cpu` | Presentation backend; GPU uses SDL3 GPU | GPU needs game/compare and `F3RT_GPU` build support. Headless still uses CPU. |
| `--video-interp` | `off`, `linear` or `fit` | `off` | Separate opt-in GPU PF2 water/board line sampling | Non-off needs `--video-backend gpu`; scale 1 and headless remain unchanged. Does not interpolate sprites. |
| `--netplay-server` | `HOST:PORT` | none | Address of the relay server | Any `--netplay-*` flag turns netplay on. |
| `--netplay-room` | `CODE` | none | Room name on the relay server | Required in netplay mode. The transport rejects an empty room. |
| `--netplay-player` | `1` or `2` | automatic | Player slot | Else: `--netplay-player must be 1 or 2`. If you omit it, the transport option stays `0` (automatic). |
| `--netplay-delay` | `0` to `8` | `2` | Input delay in frames | Else: `--netplay-delay must be 0..8`. |
| `--help` | none | | Print usage and exit with code 0 | |

The window title is `f3rt — SET`. The window size is `(320 + 2 × border) × 3` by `696` pixels at start, and you can resize it.

### Rules that the program checks after reading the flags

The program checks these rules in this order.

1. `--rom-dir` must be set, and `--dump-every` must not be `0`.
2. `--headless` needs `--frames`.
3. The sound driver must be `oracle` or `native`.
4. In `landmakr`, `--set` must be `landmakrj`.
5. The video mode must be `fdp`, `game` or `compare`.
6. `game` and `compare` need `--set landmakrj`, native execution and no `--allow-fallback`.
7. The video filter must be `nearest` or `linear`.
8. In `fdp` mode, a scale other than 1, a border other than 0, or the filter `linear` is an error: `Presentation enhancements require --video game or compare`.
9. In netplay mode, the server and the room must be set.
10. Instrumented collection requires strict native main and sound CPUs. Profile-slim rejects interpreter execution, `--allow-fallback` and oracle sound.

### Netplay mode rules

Netplay starts when you pass at least one `--netplay-*` flag. The program then requires all of these conditions:

- Native execution (`--translated` or the `landmakr` default) and no `--allow-fallback`.
- `--video game` with scale 1 and border 0.
- `--sound-driver native`.
- No `--eeprom`, no `--sound-trace` and no `--fallback-report`.
- If `--frames` is set, it must be less than 4294966271 (`UINT32_MAX - 1024`).

If one condition fails, the program stops with `Netplay requires strict-native game video/native sound at scale 1, border 0; EEPROM persistence and diagnostic traces are disabled`. In netplay mode, the program starts with a factory-reset EEPROM.

### Keyboard controls

Normal play and netplay use different key handlers (`key()` and `netplay_key()` in `runtime/frontend.cpp`).

| Key | Normal play (`key()`) | Netplay (`netplay_key()`) |
| --- | --- | --- |
| Up, Down, Left, Right | Machine input port 1, bits `1`, `2`, `4`, `8` | Bits 0 to 3 of the local input word |
| `Z`, `X`, `C` | Machine input port 0, bits `1`, `2`, `4` | Bits 4 to 6 |
| `1`, `2` | Port 0, bits `0x1000`, `0x2000` | Both set bit 7 |
| `5`, `6` | System input bits `0x10`, `0x20` | Both set bit 8 |
| `F1` | Port 0, bit `0x200` | Bit 9 |
| `F2` | System input bit `0x02` | Bit 10 |
| `Escape` | Quit | Quit |

In normal play, a pressed key clears the matching input bit, because the F3 inputs are active low. In netplay, a pressed key sets a bit in the 16-bit input word that the program sends to the peer.

When the window loses focus, the program releases all keys.

### Output lines

At exit, the program prints one line that starts with `set=`. The line contains `frames`, `pc`, `sound_pc`, `sound_driver`, `frame_crc`, `cycles`, `native_blocks`, `fallback_instructions`, `audio_frames`, `audio_peak` and `nonzero_samples`. Scripts can parse this line. With a window, the program first prints a `window_open` line. In netplay mode, the program prints `netplay_ready` when the rollback engine starts, and `netplay_confirmed=` at exit.

### Examples

```sh
# Play the game (player build)
./build/landmakr

# Run 600 frames without a window and write a WAV file
./build/f3rt-run --rom-dir roms/landmakrj --translated --headless --frames 600 --wav out.wav

# Find instructions that the recompiler did not translate
./build/f3rt-run --rom-dir roms/landmakrj --translated --allow-fallback --headless --frames 3000 --fallback-report fallback.tsv

# Two-times picture with a 16-column border
./build/landmakr --video-scale 2 --video-border 16 --video-filter linear
```

## f3rt-replay

`f3rt-replay` replays MAME captures through the video renderer or the audio core. It compares video output with the MAME reference. Source: `runtime/replay.cpp`. The program accepts only the flags below. Each flag needs a value. A flag with a missing value prints the usage line and exits with code 2.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | none | Directory that holds the ROM files | Required. The program loads the default set (`landmakrj`). |
| `--captures` | `DIR` | none | A capture directory | If the directory has `graphics.bin`, the program treats it as one frame. Else it renders every sub-directory that has `graphics.bin`, in sorted order. Exclusive with `--audio-trace`. |
| `--audio-trace` | `FILE` | none | A MAME audio trace (`F3AUD2` header) | Exclusive with `--captures`. A wrong header stops with `Invalid F3 audio trace header`. A trace without an end record stops with `Truncated audio trace: missing end timestamp`. |
| `--output` | `PATH` | none | Output directory (video) or WAV file (audio) | Required. |

Rules:

- `--rom-dir` and `--output` are required.
- You must pass exactly one of `--captures` and `--audio-trace`.
- Video mode needs these files in each frame directory: `palette.bin`, `graphics.bin`, `control.bin`, `spriteram_active.bin`, `reference.argb`.
- Video mode writes `rendered.argb` and `rendered.bmp` to `OUTPUT/FRAME_NAME/`. It prints one line for each frame with the number of wrong pixels, the maximum error and the mean error. It prints a final line `frames=N total_mismatched_pixels=M`.
- Exit code: `0` if no RGB channel differs, `1` if any RGB channel differs, `2` for an error. Alpha differences do not count.
- Audio mode prints `audio_writes=... frames=... sample_rate=... peak=...` and writes a WAV file.
- Frontend dumps are not complete replay inputs. They lack `spriteram_active.bin` and the MAME `reference.argb` image.
- Required file sizes are 32768 bytes for palette, 262144 for graphics, 32 for control, 65536 for sprites and 296960 for reference.

```sh
./build/f3rt-replay --rom-dir roms/landmakrj --captures captures/landmakrj_attract --output build/replay
./build/f3rt-replay --rom-dir roms/landmakrj --audio-trace captures/attract.f3aud --output build/replay.wav
```

## f3rt-gameplay-regression

This program runs the strict native game for many frames with a seeded input schedule. It stops with an error if the CPU halts or if a fallback instruction runs. Source: `tools/gameplay_regression.cpp`. It needs a build with `F3_GENERATED_DIR`.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | `F3RT_DEFAULT_ROM_DIR` if compiled in | ROM directory | Else: `ROM directory must be specified ...`. |
| `--set` | `NAME` | `landmakrj` | ROM set | Only `landmakrj` is valid. |
| `--seed` | `N` | `12345` | Seed of the input generator | If you do not pass the flag, the program reads the environment variable `SEED` (any base that `strtoull` accepts). |
| `--frames` | `N` | `40000` | Number of frames to run | `0` is an error: `--frames must be positive`. |
| `--dump-dir` | `DIR` | none | Write the final machine state, or the state at the first video difference | Same file set as `--dump-dir` in the frontend. |
| `--capture-surface` | `BMP` | none | Save the final frame as a BMP file | `--surface` is an alias. The program creates missing parent directories. |
| `--sound-trace` | `FILE` | none | Record the sound CPU and mailbox bus events | |
| `--sound-driver` | `oracle` or `native` | `oracle` | Sound CPU implementation | Unlike the frontend, the default does not change when the sound program is generated. `native` needs `F3RT_SOUND_GENERATED`. |
| `--wav` | `FILE` | none | Save the audio | |
| `--profile-out` | `FILE` | none | Merge actual generated main/sound entry counts, with periodic and exit flush | Requires instrumentation and `--sound-driver native`; one file per concurrent collector. |
| `--video-diff` | none | off | Compare the game-data renderer with the FDP renderer | Starts at frame 600. |
| `--video-layer-mask` | `N` | `511` | Layers to compare | Bits 0 to 3: playfields. Bits 4 to 7: sprites. Bit 8: text. A value of `0` or a value above `511` is an error. Accepts `0x` hex. |
| `--video-diff-every` | `N` | `120` | Compare every N frames | `0` is an error. |
| `--help`, `-h` | none | | Print usage | |

The input schedule uses the shared constants in `tools/gameplay_inputs.hpp` (`f3rt::test::ScheduleConfig`). On success, the program prints one line that starts with `SUCCESS` and contains `seed`, `frames`, `frame_crc`, `cycles`, `fallback_instructions`, the audio statistics and `fps`. Exit code: `0` on success, `1` on failure.

```sh
./build/f3rt-gameplay-regression --seed 3 --frames 40000 --video-diff
```

## f3rt-sound-extract

This program freezes the main CPU after boot. It then injects sound packets into the sound driver and records the audio. Source: `tools/sound_extract.cpp`. It is always built. Native sound needs the generated sound program.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | `F3RT_DEFAULT_ROM_DIR` if compiled in | ROM directory | Else the program stops. |
| `--set` | `NAME` | `landmakrj` | ROM set | |
| `--packet` | `HEX` | none | A packet to inject at the start of the event (time 0) | You can repeat the flag. White space in the string is ignored. The string must have an even length. Byte 0 must equal the packet size. The minimum size is 2 bytes. |
| `--at` | `SECONDS:HEX` | none | A packet to inject at a time after the event start | You can repeat the flag. The time must be 0 or more. All packets must be earlier than `--seconds`. |
| `--seconds` | `DURATION` | `5.0` | Time to advance the audio after the event start | Must be a positive number. |
| `--boot-frames` | `N` | `900` | Number of interpreted boot frames before the main CPU freezes | Must be a positive integer. The sound CPU must be out of reset after boot. Else: `Sound CPU is still held in reset; increase --boot-frames`. |
| `--sound-trace` | `FILE` | none | Record a bus trace from cold boot | |
| `--wav` | `FILE` | none | Write the audio as a 16-bit stereo WAV | |
| `--wav-window` | `full` or `event` | `full` | What the WAV contains | `full`: boot plus extraction. `event`: only the time after the event start. |
| `--sound-driver` | `oracle` or `native` | `oracle` | Sound CPU implementation | |
| `--help`, `-h` | none | | Print usage | |

The program writes packets into the mailbox ring buffer at main address `0xc00000` (1024 bytes). It stops with `Mailbox full at scheduled tick; space commands farther apart` if there is no room. On success, it prints `SUCCESS driver=... packets=...`.

```sh
./build/f3rt-sound-extract --sound-driver native --packet 038001 --seconds 3 --wav-window event --wav note.wav
```

## f3rt-netplay-oracle

This program proves that snapshots and netplay give the same result as a single local machine. It has three modes. Source: `tools/netplay_oracle.cpp`. It needs a build with `F3_GENERATED_DIR`. Only the `landmakrj` set is valid.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--mode` | `snapshot`, `reference` or `client` | `reference` | Run mode | `snapshot-proof` is an alias of `snapshot`. Other values stop with `Unknown --mode`. |
| `--rom-dir` | `DIR` | `F3RT_DEFAULT_ROM_DIR` | ROM directory | Required if the build has no default. |
| `--set` | `NAME` | `landmakrj` | ROM set | Other values stop: `netplay_oracle requires landmakrj ROM set`. |
| `--seed` | `N` | `12345` | Input schedule seed | |
| `--frames` | `N` | `20000` | Number of frames | `0` is an error. |
| `--schedule` | `versus` or `single` | `versus` | Input schedule type | |
| `--sound-driver` | `native`, `oracle` or `all` | `native` | Sound CPU implementation | `all` runs `native` and then `oracle`. It is for `snapshot` mode. |
| `--player` | `1` or `2` | none | Player slot | Required in `client` mode. |
| `--server` | `HOST:PORT` | `127.0.0.1:9000` | Relay server | `client` mode. |
| `--room` | `NAME` | `oracle_room` | Relay room | `client` mode. |
| `--delay` | `N` | `2` | Input delay in frames | `reference` and `client` modes. |
| `--window` | `N` | `16` | Rollback prediction window in frames | `client` mode. |
| `--timeout` | `SEC` | `120` | Time limit in seconds | `client` mode. |
| `--unthrottled` | none | off | Do not sleep while the client waits | `client` mode. |
| `--snapshot-interval` | `N` | `1000` | Frame step between snapshot test points | `snapshot` mode. `0` is an error. The test points are 0, N, 2N, and so on, below `--frames`. |
| `--snapshot-k` | `N` | `0` | One resimulation depth | `0` runs the depths 1, 7, 16, 31 and 97. |
| `--video-scale` | `N` | `1` | Video scale | The oracle does not check the range. |
| `--video-border` | `N` | `0` | Video border columns | The oracle does not check the range. |
| `--surface` | `BMP` | none | Save the final frame | `--capture-surface` is an alias. |
| `--dump-dir` | `DIR` | none | Write machine state | |
| `--dump-every` | `N` | `0` | Write a frame BMP every N frames | `0` turns it off. |
| `--stall-at` | `FRAME` | `0` | Pause the client completely at this frame | Use with `--stall-ms`. |
| `--stall-ms` | `MS` | `0` | Length of the pause | |
| `--withhold-input-at` | `FRAME` | `0` | Hold back local input from this frame | Use with `--withhold-input-ms`. |
| `--withhold-input-ms` | `MS` | `0` | Length of the hold | |
| `--observe-event-at` | `FRAME` | `0` | Measure corrections and stalls near this frame | |
| `--corrupt-build-hash` | none | off | Change the build hash to test the handshake refusal | |
| `--help`, `-h` | none | | Print usage | |

`client` mode stops with `Client mode requires --player 1 or --player 2` if the player is wrong. See [Tools](/reference/tools) for the script that runs the full test suite.

## f3rt-gpu-regression

Strict-native Land Maker CPU-versus-GPU presentation diagnostics, built with
`F3RT_GPU`, SDL3 and generated main/sound programs. No interpolation in the
parity comparison. Native machine/audio output remains CPU-produced.

```sh
./build/f3rt-gpu-regression --seed 5 --frames 4000 --scale 4 --border 48 --layers
./build/f3rt-gpu-regression --seed 5 --frames 4000 --scale 4 --border 48 --bench
./build/f3rt-gpu-regression --seed 5 --frames 1560 --scale 4 --border 48 --interp fit --capture-frame 1500 --capture-frame 1560 --dump-dir /tmp/f3-water --bench
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--rom-dir DIR` | Configured ROM directory | Japan 2.01J ROM set |
| `--seed N` | `12345`, or `SEED` environment | Deterministic single-player input schedule |
| `--frames N` | `4000` | Native frames to execute |
| `--scale N`, `--border N` | `1`, `0` | Scale 1..4; border 0..160 |
| `--every N` | `1` | Sample every N frames; always sample the final frame |
| `--layers` | Off | Compare all nine isolated supported layers plus the composite |
| `--bench` | Off | Varied-scene timings after 600 frames; 100 serial/threaded/GPU repeats of the last supported frame |
| `--dump-dir DIR` | None | External CPU/GPU PNGs and native BMPs |
| `--interp off\|linear\|fit` | `off` | Second opt-in renderer; CPU parity comparison always uses the off renderer |
| `--capture-frame N` | None | Repeatable; forces selected off/mode PNGs and complete PF2 row CSVs, even off the sampling interval; needs `--dump-dir` |
| `--inject-frame N` | `1407` | Supported baseline for diagnostic branches |
| `--inject-bitmap`, `--inject-trails`, `--inject-globalflip` | Off | Induce actual unsupported-mode boundaries and verify oracle presentation/recovery |
| `--inject-unknown`, `--inject-ending` | Off | Induce unsupported writer/ending-producer boundaries; not a played-through ending |
| `--sound-driver native` | Native | Only native sound is accepted |

GPU timings include upload, submission, fence wait and readback. Normal window
presentation does not read back. Both rendering paths are checked against
canonical snapshot bytes; trails additionally compare an independent CPU-backend
branch. Fallback comparisons bypass layer masks and are counted only as full
frames. `PARITY`, per-layer mismatch counts, `BENCH` and `SUCCESS` lines report
the exercised coverage. Any mismatch or CPU fallback exits nonzero.

Interpolation diagnostics compare every declined/oracle image and all rows
outside the validated geometry run (including whole boundary rows) against
off. Accepted scenes also isolate sprites and exercise induced invalid-run,
garbage-input, jump, residual and palette-continuity cases. Logs identify the
known-ROM-profile source or rejection reason per scene transition/requested
frame and summarize sampled scene counts. `--bench` additionally reports
opt-in fenced timings; choose a final water frame such as 1560 to benchmark an
actually accepted effect rather than a declined later scene.

## f3rt-check

`f3rt-check` is the unit-level self-test of the runtime devices. It takes **no arguments**. It does not read ROM files. It builds only when `BUILD_TESTING` is on (the default after `include(CTest)`). CTest registers it as the test `runtime-devices`.

```sh
ctest --test-dir build --output-on-failure
./build/f3rt-check
```

On success it prints `PASS memory/lanes, input/coin, ...` and exits with code 0. On failure it prints `FAIL` and the failed check, and exits with code 1.

## python3 -m recomp

The recompiler runs from the repository root. Source: `recomp/__main__.py`. The package needs `capstone==5.0.9` (`recomp/requirements.txt`).

```sh
python3 -m recomp discover --config games/landmakrj/config.toml --rom-dir roms/landmakrj --output build/landmakrj
python3 -m recomp emit --config games/landmakrj/config.toml --rom-dir roms/landmakrj --output build/landmakrj
```

| Item | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| Command (positional) | `discover` or `emit` | none | `discover` finds code and writes `coverage.json`. `emit` does that and then writes the C program. | Required. |
| `--config` | `PATH` | none | Game config file (TOML) | Required. See [Game config](/reference/game-config). |
| `--rom-dir` | `DIR` | none | Directory with the ROM lane files | Required. |
| `--output` | `DIR` | none | Output directory | Required. The program creates it. |
| `--max-block-instructions` | `N` | `32` | Maximum instructions in one native block (and the page size in `all_aligned` mode, as `N × 2` bytes) | Used by `emit` only. A value below 1 raises `block and shard sizes must be positive`. |
| `--profile-tiers` | `PROFILE` | none | Split emitted hot/cold units without losing any executable entry | Matching CRC/base/size required; mutually exclusive with slim. |
| `--profile-slim` | `PROFILE` | none | Retain only observed entry addresses | Explicit opt-in; matching ROM identity required. Missing dispatch aborts in the paired slim runtime. |

On success the program prints a JSON summary and exits with code 0. `OSError`, `ValueError`, `KeyError` and `ImportError` give the message `f3-recomp: ...` on stderr and exit code 1. See [Generated files](/reference/generated-files) for the output.

## tools/compile_sound.py

This script compiles the sound CPU ROM to C. CMake runs it when `F3_ROM_DIR` is set. Source: `tools/compile_sound.py`. It needs `capstone`.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | none | Directory with the sound ROM chips | Required. Looks for `e61-14.32` and `e61-15.33` (or `sound.bin`). The CRC32 of the combined image must be `5a7e9117`. |
| `--output` | `DIR` | none | Output directory | Required. |
| `--blocks-per-file` | `N` | `1024` | Number of block functions in each C file | |
| `--coverage` | `all_aligned` | `all_aligned` | Coverage mode | The only allowed value. The script compiles every even address of the sound ROM. |
| `--profile-tiers` | `PROFILE` | none | Split sound units into hot/cold compilation tiers | Full aligned table retained; mutually exclusive with slim. |
| `--profile-slim` | `PROFILE` | none | Emit only profiled sound statements and a sparse sorted table | Requires paired slim runtime; no interpreter fallback for omitted entries. |

```sh
python3 tools/compile_sound.py --rom-dir roms/landmakrj --output build/generated/sound-landmakrj
```

## tools/block_profile.py

`merge --output PROFILE RUN.profile ...` unions ROM identities and entry addresses,
summing hit/miss counts with uint64 saturation. Duplicate input paths are rejected.
Use separate files for simultaneous collectors; the runtime rejects a second
writer to the same path. Sequential runs automatically merge their output.

`report PROFILE --main-generated DIR --sound-generated DIR [--binary MACH_O]
[--output JSON]` reports original/retained/executed entry counts and active
generated-C bytes by descriptive ROM region. Optional Mach-O measurements include
file size, __TEXT size and native function spans; function-span attribution includes
alignment/page packing and does not assign shared/runtime __TEXT overhead to ROMs.
The Japanese padding and gfx-looking bins are descriptive, never exclusion proofs.

Profile version 1 is ASCII. `rom CPU CRC BASE SIZE` defines a CPU/ROM identity;
`hit CPU CRC ADDRESS COUNT` records execution, and `miss ...` records a slim abort.
CRC, base, size and address are eight hexadecimal digits; counts are positive
decimal uint64 values. The header is `F3-BLOCK-PROFILE 1`. Both compilers reject
incompatible versions or CRC/base/size. A miss is not hot until a full profiling
build actually executes that entry. No profile contains ROM bytes.


## netplay/server (relay)

The relay server is a Go program. It forwards UDP packets between two clients in a room. It can also add network faults for tests. Source: `netplay/server/main.go`. Build it with `go build`. The test scripts expect the binary at `build/netplay-server`.

```sh
cd netplay/server
go build -o ../../build/netplay-server .
../../build/netplay-server -addr 0.0.0.0:9000
```

Go flags accept one or two leading dashes (`-addr` or `--addr`). Duration values use Go syntax, for example `40ms` or `1s`.

Go also accepts `-flag=value`. Use `-h` or `--help` for usage; Go exits with code 0. Invalid flags or values exit with code 2.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `-addr` | `HOST:PORT` | `127.0.0.1:9000` | UDP listen address | The default accepts only local clients. Use `0.0.0.0:PORT` for remote clients. |
| `-port` | `N` | `0` | UDP port | If greater than 0, it replaces `-addr` with `127.0.0.1:N`. |
| `-rtt` | duration | `0` | Simulated round-trip time | Used only if `-delay` is 0. The one-way delay is `rtt / 2`. |
| `-delay` | duration | `0` | Simulated one-way delay | |
| `-jitter` | duration | `0` | Simulated jitter | Each packet gets a random change between minus and plus this value. The total delay does not go below 0. |
| `-loss` | float | `0.0` | Packet loss rate, 0.0 to 1.0 | The program does not check the range. |
| `-reorder` | float | `0.0` | Rate of packets that get extra delay | The extra delay is `delay + jitter`, or 20 ms if both are 0. |
| `-duplicate` | float | `0.0` | Rate of duplicated packets | |
| `-dup` | float | `0.0` | Alias of `-duplicate` | Used only if `-duplicate` is 0. |
| `-seed` | int | `0` | Seed of the fault generator | `0` means a random seed. A fixed seed makes the faults repeatable. |

If any fault value is greater than 0, the server logs `Network impairment enabled: ...`. The impairment queue holds at most 2048 packets (`MaxImpairmentQueue`). The server drops packets when the queue is full. The server stops on `Ctrl-C` or `SIGTERM`. See [Netplay server](/developer/netplay/server) for the internals.

## Python tools

All tools below run from the repository root with `python3`. See [Tools](/reference/tools) for a short description of each tool.

### tools/run_gameplay_regression.py

This script runs `f3rt-gameplay-regression` once for each seed. It fails if any seed fails.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--binary` | `PATH` | `build/f3rt-gameplay-regression` | Program to run | The script stops if the file does not exist. |
| `--rom-dir` | `DIR` | none | Passed on as `--rom-dir` | If omitted, the binary uses its built-in default. |
| `--seeds` | `N [N ...]` | `1 2 3 4 5 6 7 8` | Seeds to run | Each seed must fit in an unsigned 64-bit number. |
| `--frames` | `N` | `40000` | Frames for each seed | Must be positive. |
| `--dump-captures-dir` | `DIR` | none | Save `seed_N.bmp` for each seed | The script creates the directory. It passes `--surface` to the binary. |
| `--video-diff` | none | off | Turn on video comparison | |
| `--video-layer-mask` | `N` | `511` | Layer mask (accepts `0x` hex) | Must be 1 to 511. |
| `--video-diff-every` | `N` | `120` | Compare interval | Must be positive. |

Exit code: `1` if any seed failed, else `0`.

### tools/run_netplay_oracle.py

This script starts the relay server and two oracle clients, and checks that the results match a reference run.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--oracle-bin` | `PATH` | `build/f3rt-netplay-oracle` | Oracle program | If missing, the script tries `build/native/f3rt-netplay-oracle` and `build/f3rt-netplay-oracle`. |
| `--server-bin` | `PATH` | `build/netplay-server` | Relay server program | |
| `--rom-dir` | `DIR` | none | ROM directory | If omitted, the binary uses its built-in default. |
| `--seeds` | `N [N ...]` | `1 2 3 5` | Seeds to run | |
| `--frames` | `N` | `20000` | Frames per seed | The snapshot suite uses `min(frames, 6000)`. |
| `--delay` | `N` | `2` | Input delay | |
| `--window` | `N` | `16` | Rollback window | |
| `--sound-driver` | `oracle`, `native` or `all` | `native` | Sound CPU implementation | With `all`, snapshot tests run both drivers and netplay runs `native`. |
| `--suite` | `all`, `snapshot`, `baseline`, `impaired` or `cases` | `all` | Test group to run | `cases` runs the late input, long stall, build mismatch and disconnect tests. |
| `--dump-captures-dir` | `DIR` | none | Save frame captures | |
| `--log-dir` | `DIR` | `build/netplay_logs` | Directory for the logs | |

The impaired suite uses 80 ms round-trip time, 20 ms jitter, 3% loss and 3% reorder (from the script header and its output text).

### tools/compare_frames.py

This script compares two frames, or two directories of frames, pixel by pixel. It uses only the Python standard library.

| Item | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `reference` (positional) | file or directory | none | Reference frame (MAME) | BMP, or raw `.argb`, `.bgra`, `.rgba`, `.raw`. |
| `actual` (positional) | file or directory | none | Frame from f3rt | Both arguments must be directories for directory mode. |
| `--width` | `N` | `320` | Width of raw buffers | |
| `--height` | `N` | `232` | Height of raw buffers | |
| `--format` | `bgra`, `argb`, `rgba`, `rgb` or `bgr` | `bgra` | Byte order of raw buffers | `bgra` is the MAME output on little-endian hosts. |
| `--tolerance` | `N` | `0` | Largest channel difference that still counts as equal | |
| `--max-error-allowed` | `N` | `0` | Largest error that gives exit code 0 | |
| `--diff` | `PATH` | none | Write a diff BMP | Single-frame mode. |
| `--diff-dir` | `DIR` | none | Write diff BMPs | Directory mode. |
| `--diff-all` | none | off | Write a diff BMP for every frame, not only for frames that differ | |
| `--diff-amp` | `N` | `8` | Amplification factor of the diff image | |
| `--json` | none | off | Print the result as JSON | |
| `--quiet` | none | off | Do not print the text report | |

Exit code: `0` if equal, `1` if a mismatch exists, `2` for an error.

### tools/compare_audio.py

This script compares two 16-bit stereo WAV files with one fixed delay correction. It needs NumPy and SciPy.

| Item | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `reference` (positional) | `PATH` | none | Reference WAV | |
| `candidate` (positional) | `PATH` | none | WAV to test | The script resamples it to the reference rate. |
| `--start` | seconds | `18.0` | Start of the compare range | |
| `--end` | seconds | end of audio | End of the compare range | |
| `--window` | seconds | `2.0` | Length of each metric window | |
| `--max-lag` | seconds | `0.25` | Largest delay that the search tries | |
| `--lag` | seconds | search | Fixed candidate delay | If omitted, the script finds the delay by correlation. A silent calibration window gives an error. |
| `--json` | `PATH` | none | Write the full report as JSON | |

A negative lag means that the candidate plays earlier than the reference.

### tools/compare_sound.py

This script compares two sound bus traces record by record. It does no lag fitting and no tolerance.

| Item | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `oracle` (positional) | `PATH` | none | Trace from the interpreted driver | |
| `model` (positional) | `PATH` | none | Trace from the other driver | |
| `--json` | `PATH` | none | Write the result to a file | |

Exit code: `0` if the traces are equal, else `1`.

### tools/decode_sound.py

This script decodes an F3SND2 trace into JSON lines.

| Item | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `trace` (positional) | `PATH` | none | F3SND2 trace | |
| `--output` | `PATH` | none | Output file | Required. If the name ends in `.gz`, the script compresses it. |
| `--notes-only` | none | off | Keep only voice starts, commands, resets and the end record | |
| `--commands-only` | none | off | Keep only submitted and consumed command packets | Takes priority over `--notes-only`. |

The script prints a JSON object with the count of each event type.

### tools/differential (run.py and `python3 -m tools.differential`)

The differential harness compares the recompiler output for random 68020 instructions against the Musashi reference core. `tools/differential/run.py` and `python3 -m tools.differential` run the same `main()` function.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--musashi` | `DIR` | search | Musashi source directory | The directory must contain `m68k.h`. The default search order is `runtime/third_party/musashi`, then `../runtime/runtime/third_party/musashi`. |
| `--output` | `DIR` | `build/differential` | Output directory for generated C and binaries | |
| `--cases` | `N` | `500` | Number of test cases | |
| `--seed` | `N` | `42` | Generator seed | |
| `--instructions` | `PATH` | none | External JSON list of instructions from discovery | |
| `--filter` | `TEXT` | none | Test only mnemonics that contain this text | |
| `--compile-only` | none | off | Build the runner but do not run it | |
| `-v`, `--verbose` | none | off | Verbose output | |

The harness uses the environment variables `CC` (default `clang`) and `AR` (default `ar`). Exit code `2` means an error such as a missing Musashi directory.

### tools/differential/export_cycles.py

This script writes `recomp/68020_cycles.csv` from a Musashi `m68kops.c` file.

| Item | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `generated_ops` (positional) | `PATH` | none | The generated `m68kops.c` | |
| `--output` | `PATH` | `recomp/68020_cycles.csv` | Output CSV | |

### tools/mame/stage_roms.py

This script checks the CRC32 of the ROM files and packs them into a ZIP archive for MAME. It pads the two 128 KiB sound ROMs to 256 KiB with `0xFF`.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--source` | `PATH` | search | Directory or ZIP with the game ROMs | If omitted, the script tries a fixed list of paths in `DEFAULT_SEARCH_PATHS`. Those paths are specific to the author's machine. |
| `--board-source` | `PATH` | `/Users/darien/Workspace/f3-stuff/roms/puchicar` | Directory or ZIP with the F3 board PLD files | The default is specific to the author's machine. Set it on your machine. |
| `--out-zip` | `PATH` | `tools/mame/staged_roms/landmakrj.zip` | Output ZIP | |
| `--out-dir` | `DIR` | none | Also write the files to a directory | |
| `--check-only` | none | off | Check CRC32 and write nothing | |

Exit code `1` if a ROM is missing or wrong.

### tools/mame/run_capture.sh

This Bash script stages the ROMs and runs MAME with `capture.lua` to record attract-mode frames.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--mame` | `PATH` | `$MAME_BIN`, else a path on the author's machine | MAME executable | If unavailable, the script tries its fixed `mamef3` path, then `f3` and `mame` in `PATH`. It does not search `PATH` for `mamef3`. |
| `--rompath` | `PATH` | staged ROMs and two author paths | MAME `-rompath` value | |
| `--outdir` | `DIR` | `captures/landmakrj_attract` | Output directory | |
| `--start-frame` | `N` | `300` | First frame to capture | |
| `--count` | `N` | `10` | Number of frames | |
| `--step` | `N` | `1` | Capture every N frames | |
| `--wav` | `PATH` | `OUTDIR/attract.wav` | WAV file from MAME | |
| `--throttle` | none | off | Run at real speed | The default is `-nothrottle`. |
| `--stage-only` | none | off | Stage the ROMs and exit | |
| `--dry-run` | none | off | Print the MAME command without launching MAME | The script still creates the output directory, stages missing ROMs and checks for a MAME executable. |
| `-h`, `--help` | none | | Print usage | |

The script sets these variables for `tools/mame/capture.lua`: `F3_CAPTURE_DIR`, `F3_CAPTURE_START`, `F3_CAPTURE_COUNT`, `F3_CAPTURE_STEP` and `F3_CAPTURE_EXIT=1`. If you run `capture.lua` yourself, `F3_CAPTURE_EXIT=0` keeps MAME open. `tools/mame/audio_trace.lua` needs the variable `F3_AUDIO_TRACE` (output path).

Stage ROMs explicitly with `stage_roms.py --source PATH --board-source PATH` before capture on a new computer. `run_capture.sh` does not forward these options. It reuses an existing staged ZIP without rechecking it. The shell parser requires separate values. A missing value causes a Bash unset-variable error.

## Environment variables

| Variable | Used by | Meaning |
| --- | --- | --- |
| `SEED` | `f3rt-gameplay-regression` | Seed, if `--seed` is absent |
| `MAME_BIN` | `tools/mame/run_capture.sh` | MAME executable |
| `CC`, `AR` | `tools/differential` | C compiler and archiver |
| `F3_CAPTURE_DIR`, `F3_CAPTURE_START`, `F3_CAPTURE_COUNT`, `F3_CAPTURE_STEP`, `F3_CAPTURE_EXIT` | `tools/mame/capture.lua` | Capture settings |
| `F3_AUDIO_TRACE` | `tools/mame/audio_trace.lua` | Audio trace output path |
| `PYTHONPATH` | CMake configure step | CMake adds `build/python` so that Python finds `capstone` |
