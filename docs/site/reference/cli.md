# Command-line reference

This page lists the project commands and their arguments. The tables give defaults, value limits and error behavior.

## General rules

- The C++ parsers use separate arguments: `--flag value`, not `--flag=value`.
- Frontend options with missing values report `Missing value for ...`. Unknown frontend options report `Unknown argument: ...`.
- Other programs have their own error messages. For example, `f3rt-replay` reports usage for unknown options and missing values.
- C++ numeric options use conversions such as `stoull`. Invalid text raises a conversion error. These conversions can accept a numeric prefix with trailing text.
- The ROM directory must contain the selected title's manifest chips. See [Game config](/reference/game-config); main and sound image CRC guards reject mismatched generated code.
- Build targets and their default values come from CMake. See [Build options](/reference/build-options).

| Program | Source | Exit code on error |
| --- | --- | --- |
| `landmakr`, `f3rt-run` | `runtime/frontend/frontend.cpp` | 1 (message starts with `f3rt:`) |
| `f3rt-replay` | `runtime/replay.cpp` | 2 |
| `f3rt-tool` | `tools/tool.cpp`, `tools/commands/*.cpp` | 1 (or subcommand error) |
| `f3rt-test-<area>` | `runtime/tests/<area>.cpp` | 1 (GoogleTest / RapidCheck failure) |

## landmakr and f3rt-run

All title executables use `runtime/frontend/frontend.cpp`: `F3_GAME` selects `landmakr`
(for either LM config), `rayforce`, `commandw` or `ridingf`. The selected title
build defaults to strict-native main and generated native sound with Reference
audio devices. `landmakr` defaults to Japan; World remains config-only/unverified.
`f3rt-run` is diagnostic. New titles require FDP video and Reference audio:
game-data video, Enhanced audio and enhanced GPU/motion semantics remain Japan-only.
See [porting](/developer/porting) for exact revisions and finite evidence.

### Differences between the two builds

| Item | Selected title (`F3RT_GAME`) | `f3rt-run` |
| --- | --- | --- |
| `--rom-dir` default | Configured `F3_ROM_DIR` | Required explicitly |
| Native execution | On | Off; pass `--translated` with linked main code |
| `--allow-fallback` | Off | On, diagnostic interpretation |
| `--set` | Must match selected `F3_GAME` | Defaults to selected `F3_GAME`; manifests accept all five sets, but native code must match the loaded image |
| Video default | `game` for strict-native Japan; `fdp` for other titles | `fdp` |
| Reference sound driver default | `native` with generated sound, otherwise `oracle` | Same rule |
| Generated main | Always | Only with `F3_GENERATED_DIR` |

Accepted sets are `landmakrj`, `landmakr`, `rayforce`, `commandw`, `ridingf`.
A set flag is not a way to run another revision through mismatched native code.

Audio defaults to `--audio-backend enhanced` for `landmakrj`; every other set uses `reference`. Enhanced provides approximate audio using a ROM sequencer and PCM synthesizer on its own worker at 48 kHz without executing the sound CPU or the ES chips. `--audio-backend reference` runs the cycle-accurate sound CPU and chip emulation path (using recompiled native sound automatically when generated, otherwise interpreted).

### What changes with --allow-fallback

The fallback is the 68020 interpreter. The interpreter runs instructions that the recompiler did not translate. It is only for diagnosis.

| Effect of `--allow-fallback` | Detail |
| --- | --- |
| Machine setting | Sets `Machine::allow_main_fallback` to true. Without it, an untranslated instruction stops the program with `Untranslated main CPU instruction at PC ...`. |
| Default renderer in `landmakr` | Becomes `accurate`. The game-data renderers need the strict native path. |
| `--renderer enhanced`, `game-cpu`, `compare-cpu` or `compare-gpu` | Rejected with `Game-data video requires strict native execution`. |
| Counting | The program counts fallback instructions. `--fallback-report` writes the count for each address. |

### Flags

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | `landmakr`: `F3RT_DEFAULT_ROM_DIR`. `f3rt-run`: none | Directory that holds the ROM files | An omitted or empty directory fails validation: `--rom-dir required; --dump-every must be positive`. A missing argument reports `Missing value for --rom-dir`. |
| `--set` | `landmakrj`, `landmakr`, `rayforce`, `commandw`, `ridingf` | Selected `F3_GAME` | ROM set to load | Title binaries require their selected set; loaded main/sound CRCs must match generated code. |
| `--frames` | `N` | `0` | Stop after N video frames | `0` means no limit. A window run stops when you close the window or press Escape. |
| `--headless` | none | off | Do not open a window and do not open audio | Requires `--frames`. Without it: `Headless execution requires --frames`. |
| `--no-audio` | none | audio on | Do not open the audio device | The audio is still generated. `--wav` still receives the samples. |
| `--translated` | none | `landmakr`: on. `f3rt-run`: off | Run the recompiled native code | Needs a build with a generated program. Else: `This binary was built without F3_GENERATED_DIR`. |
| `--allow-fallback` | none | `landmakr`: off. `f3rt-run`: on | Let the interpreter run untranslated instructions | Diagnostic only. See the table above. |
| `--unthrottled` | none | throttled | Run as fast as possible | Without it, the program waits so that the game runs at the real frame rate. |
| `--eeprom` | `FILE` | none | EEPROM file | Loads at start if present and writes at normal exit; exactly 128 bytes required. |
| `--fast-boot` | `on` or `off` | `on` for a window; headless needs an explicit `on` | Run the power-on frames without presentation and with discarded audio, using a locally generated initialised EEPROM | Host-side only: no ROM patch, CPU start change or game RAM write; with the same EEPROM contents and inputs, `frame_crc` matches an ordinary run. Disabled by `--wav`, `--dump-dir`, `--sound-trace`, `--profile-out`, `--fallback-report`. Saves the preference; explicit CLI overrides it. |
| `--boot-cache` | `on` or `off` | `off` | Also store the post-boot state in `boot/<set>.state` and skip the boot frames on later launches | Needs fast boot and no `--eeprom`. A cache is loaded only when the build, ROMs, state format and presentation geometry match; otherwise it is ignored, the boot runs normally and the cache is rewritten. Saves the preference. |
| `--wav` | `FILE` | none | Write the generated audio to a 16-bit stereo WAV file | Reference audio uses the audio core's rate; Enhanced audio uses 48 kHz. |
| `--audio-backend` | `enhanced` or `reference` | `enhanced` | Choose the threaded ROM sequencer/PCM synthesizer (`enhanced`) or emulated hardware devices (`reference`) | Default `enhanced` runs at 48 kHz without sound CPU or ES chip execution; when unavailable (currently for sets other than `landmakrj`), the default resolves to `reference`. `reference` executes cycle-accurate chip models and uses native recompiled sound automatically when compiled in. CLI audio selections override saved backend preferences. |
| `--sound-trace` | `FILE` | none | Write a bus trace of the sound CPU in the F3SND2 format | Reference audio only. Decode the trace with `tools/decode_sound.py`. |
| `--profile-out` | `FILE` | none | Merge versioned main/sound entry counts, atomically flush every 30 seconds and on exit | Requires `F3_PROFILE_INSTRUMENT=ON` and strict native main/sound execution. Relative destinations are fixed against the startup working directory, including after later cwd changes. Slim builds also accept it for immediate `miss` records; cold aborts always append a durable `.cold-hits` log (or `f3-cold-hits.log` without this flag). Concurrent writers must use separate paths and merge later. |
| `--discovery-log` | `FILE` | none | Record game routines the video HLE does not know about, from normal play or headless: sprite-RAM writes outside emit units from PCs not in `[video.frame_writers]` (`sprite-stray`), graphics/control writes from PCs without a documented producer (`video-write`) and oracle fallback frames (`video-fallback`) | Host-only and observe-only: cycles, `native_blocks` and CRCs equal a run without it. Each unique PC is written (and flushed) once on first sight with frame, address and wall time, with a summary at exit; see [the discovery log](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/WORKFLOWS.md#discovery-log). `sprite-stray` needs strict native execution and a game with emit units; `video-fallback` needs a game-data renderer. |
| `--inputs` | `FILE` | none | Press buttons and write main RAM from a script: per inclusive range of 1-based frames, hold controls, mash seeded random presses, or `poke ADDR.b\|.w\|.l=VALUE` into main RAM before the frame runs; buttons are OR'd with live input | Host-only; turns fast boot off. Pokes accept only `0x400000..0x41ffff`. Format: [scripted input](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/WORKFLOWS.md#scripted-input). Errors name the file and line. |
| `--watch-entries` / `--watch-log` | `HEX,HEX` / `FILE` | none | Log `ENTRY pc=… frame=… hits=… total=…` for each frame in which a listed main-CPU instruction executed | Needs an `F3_PROFILE_INSTRUMENT=ON` build and `--profile-out` (the counters live in the profile session). Both flags go together. Host-only and observe-only. Lines are flushed as written. |
| `--indirect-log` | `FILE` | none | Log observed computed `jmp`/`jsr` sites and targets: header `# f3rt indirect targets v1`, lines `INDIRECT site=0x%06x target=0x%06x count=N` | Needs an `F3_PROFILE_INSTRUMENT=ON` build and `--profile-out` (the counters live in the profile session). Host-only and observe-only. Flushed every 30 seconds and on exit. |
| `--fallback-report` | `TSV` | none | Write a tab-separated list of `pc` and `count` for every instruction that used fallback | |
| `--dump-dir` | `DIR` | none | Write the machine state to `DIR/frame_NNNN/` | Files: `palette.bin`, `graphics.bin`, `control.bin`, `mainram.bin`, `shared.bin`, `rendered.argb`, `rendered.bmp`, `cpu.json`, and `sprite_writers.bin` (4096 × u32 little-endian PCs attributing the last writer of each 16-byte sprite RAM entry). |
| `--dump-start` | `N` | `1` | First frame to dump | |
| `--dump-every` | `N` | `1` | Dump every N frames | `0` is an error. |
| `--surface` | `BMP` or GPU `PNG` | none | Save the CPU window surface or GPU internal-resolution image at `--frames` | Window runs only. Native dumps remain CPU-produced. |
| `--renderer` | `accurate`, `enhanced` (user-facing); `game-cpu`, `compare-cpu`, `compare-gpu` (developer-only) | `enhanced` in `landmakr` (strict native, GPU-enabled build); otherwise `accurate` | Choose the renderer. Saved as `renderer=` in the settings file (only `accurate` or `enhanced`) and in F1 → Video | `accurate` is the MAME-derived video-memory reference (CPU), not physical-chip verification. `enhanced` is the game-data renderer composited on the GPU; the only renderer with scale/border/interpolation/post-processing and display-rate presentation. `game-cpu` is the same scene composited on the CPU (the parity baseline for native pixels/CRCs). `compare-cpu`/`compare-gpu` also check supported game-renderer frames against the reference. If the renderer came from the default or settings file and the GPU cannot start, the frontend warns and falls back to `accurate`; an explicit `--renderer` never falls back. Everything except `accurate` needs strict native `landmakrj`; `enhanced`/`compare-gpu` open a window only with `F3RT_GPU` build support (headless always works). Other value: `--renderer must be accurate, enhanced, game-cpu, compare-cpu or compare-gpu`. Presentation is never part of native state: all renderers give identical frame CRCs, cycles and native blocks. |
| `--video-scale` | `1` to `4`, `auto` or `auto-integer` | `1` | Fixed or window-pixel-following internal scale | Auto modes require `enhanced` or `compare-gpu`. `auto` ceil-fits then aspect-scales with the selected filter; `auto-integer` floor-fits then presents exact nearest integer pixels with black bars. All clamp to 1–4; border counts in the fit. Headless auto stays at scale 1. |
| `--video-border` | `0` to `160` | `0` | Extra scene columns on each side of the 320-column picture | Else: `--video-border must be 0..160`. Needs a game-data renderer (not `accurate`). |
| `--video-filter` | `nearest` or `linear` | `nearest` | Texture filter for the window | Needs a game-data renderer (not `accurate`) if you choose `linear`. |
| `--video-interp` | `off`, `linear` or `fit` | `off` | Opt-in validated GPU line sampling on all four playfields | Non-off needs `enhanced` or `compare-gpu`; scale 1/headless stay exact. Both modes preserve native subrow-zero and unflagged samples. |
| `--video-interp-fields` | `none`, `geometry`, `palette`, `geometry,palette` | `geometry` | Independent geometry sampling and same-pen RGB palette-bank blending | Palette blending invents colors; alpha/clip/mosaic/priority and column jumps stay discrete. Sprite zoom already samples ROM texels at internal scale in every mode. |
| `--motion-interp` | none | off | Experimental temporal sprite, playfield/line and text-scroll interpolation at display refresh | Requires `enhanced` or `compare-gpu`. One native frame of positional latency; discontinuities snap. Headless remains canonical; unthrottled uses current geometry. Independent of `--video-interp`; Also a saved preference (F1 → Video, `motion_interp=on|off`) that applies live; the flag sets the value at launch. Two-second logs report mode/request/callback Hz, drawable submissions/s and interpolation/candidate/rejection rates. |
| `--config` | `FILE` | SDL preferences directory `settings.cfg` | Preferences path | Loads before CLI overrides; save explicitly in F1. States/screenshots use sibling directories. |
| `--volume` | `0` to `100` | `100` | Host output percentage | Does not alter simulation or confirmed PCM. |
| `--postprocess` | `off`, `crt` or `user` | `off` | GPU postprocess | Inactive unless `enhanced`; affects presentation/screenshots, not native pixels or menu. |
| `--user-shader` | `FILE.metal` or `FILE.spv` | none | User postprocess file | Metal entry `f3_postprocess`; Vulkan entry `main`. See [shader ABI/examples](/guide/video#f1-shaders-and-live-controls). |
| `--help` | none | | Print usage and exit with code 0 | |

Window geometry follows the selected crop/rotation. Japan starts at
`(320 + 2 × border) × 3` by 696 pixels; RayForce's rotated 224×320 picture
starts at 672×960. Command War/Riding Fight use unrotated 320×224 board crops.
Windows are resizable; native FDP scaling/enhanced eligibility is unchanged.

### Rules that the program checks after reading the flags

The program checks these rules in this order.

1. `--rom-dir` must be set, and `--dump-every` must not be `0`.
2. `--headless` needs `--frames`.
3. The audio backend must be `enhanced` or `reference`. Enhanced audio rejects sound tracing.
4. In a title executable, `--set` must match the selected `F3_GAME`. Enhanced audio is Japan-only (`landmakrj`).
5. The renderer must be `accurate`, `enhanced`, `game-cpu`, `compare-cpu` or `compare-gpu`.
6. Every renderer except `accurate` needs `--set landmakrj`, native execution and no `--allow-fallback`.
7. The video filter must be `nearest` or `linear`.
8. With `accurate`, a command-line scale other than 1, border other than 0, or the filter `linear` is an error: `Scale, border and filter options require --renderer enhanced (or a developer game/compare renderer)`. Saved values of these settings are ignored under `accurate`.
   Automatic scale modes and non-off interpolation require `enhanced` or `compare-gpu`, and interpolation fields must be one of the listed values.
9. Instrumented collection requires strict native main and sound CPUs. Profile-slim rejects interpreter execution, `--allow-fallback` and oracle sound.

### Keyboard controls

| Key | Default action |
| --- | --- |
| Arrows, `Z`/`X`/`C` | P1 directions and buttons |
| `1`, `5` | P1 start and coin |
| `2`, `6` | P2 start and coin (its only default bindings) |
| `F3`, `F2` | P1 service and test |
| `F1` | Menu; pauses the game |
| `F12` | PNG screenshot |
| `Escape` | Close menu, otherwise quit |
| `F11`, `Alt+Enter` | Fullscreen |

F1 remaps both keyboard/gamepad profiles. There are no default gamepad bindings. Menu capture/focus loss neutralizes held input. Save preferences explicitly. Strict-native slots 0–9 require matching build, ROMs and presentation geometry.

### Output lines

At exit, the program prints one line that starts with `set=`. The line contains `frames`, `pc`, `sound_pc`, `audio_backend`, `sound_driver`, `frame_crc`, `cycles`, `native_blocks`, `fallback_instructions`, `audio_frames`, `audio_peak` and `nonzero_samples`. Enhanced audio reports `sound_driver=none` and an additional line with `hle_commands` and `hle_rendered_frames`. Scripts can parse these lines. With a window, the program first prints a `window_open` line; a fast-boot launch prints a `fast_boot frames=... source=turbo|cache eeprom=...` line before `set=`.

### Examples

```sh
# Play Japan (default player build)
./build/landmakr

# Selected RayForce build; substitute commandw/ridingf for those title builds
./build/rayforce/rayforce --headless --frames 3600
./build/rayforce/rayforce --frames 3600 --surface build/rayforce-surface.bmp

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

## f3rt-tool gameplay

This subcommand runs the strict native game for many frames with a seeded input schedule. It stops with an error if the CPU halts or if a fallback instruction runs. Source: `tools/commands/gameplay.cpp`. It needs a build with `F3_GENERATED_DIR`.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | `F3RT_DEFAULT_ROM_DIR` if compiled in | ROM directory | Else: `ROM directory must be specified ...`. |
| `--set` | `NAME` | `landmakrj` | ROM set | Only `landmakrj` is valid. |
| `--seed` | `N` | `12345` | Seed of the input generator | If you do not pass the flag, the program reads the environment variable `SEED` (any base that `strtoull` accepts). |
| `--frames` | `N` | `40000` | Number of frames to run | `0` is an error: `--frames must be positive`. |
| `--dump-dir` | `DIR` | none | Write the final machine state, or the state at the first video difference | Same file set as `--dump-dir` in the frontend. |
| `--capture-surface` | `BMP` | none | Save the final frame as a BMP file | `--surface` is an alias. The program creates missing parent directories. |
| `--audio-backend` | `reference` or `enhanced` | `reference` | Choose emulated devices or approximate threaded Enhanced audio | `enhanced` rejects explicit `--sound-driver`, `--sound-trace` and `--profile-out`. |
| `--sound-trace` | `FILE` | none | Record the sound CPU and mailbox bus events | Reference audio only. |
| `--sound-driver` | `oracle` or `native` | `oracle` | Sound CPU implementation for Reference audio | Unlike the frontend, the default does not change when the sound program is generated. `native` needs `F3RT_SOUND_GENERATED`. `enhanced` rejects explicit driver selection. |
| `--wav` | `FILE` | none | Save the audio | |
| `--profile-out` | `FILE` | none | Merge actual generated main/sound entry counts, with periodic and exit flush | Requires instrumentation and `--sound-driver native`; one file per concurrent collector. |
| `--video-diff` | none | off | Compare the game-data renderer with the FDP renderer | Starts at frame 600. |
| `--video-layer-mask` | `N` | `511` | Layers to compare | Bits 0 to 3: playfields. Bits 4 to 7: sprites. Bit 8: text. A value of `0` or a value above `511` is an error. Accepts `0x` hex. |
| `--video-diff-every` | `N` | `120` | Compare every N frames | `0` is an error. |
| `--json` | none | off | Output machine-readable JSON result | |
| `--help`, `-h` | none | | Print usage | |

The input schedule uses `f3rt::runner::SeededScheduleInputSource`. On success, the program prints one line that starts with `SUCCESS` and contains `seed`, `frames`, `frame_crc`, `cycles`, `fallback_instructions`, the audio statistics and `fps`. Exit code: `0` on success, `1` on failure.

```sh
./build/f3rt-tool gameplay --seed 3 --frames 40000 --video-diff
```

## f3rt-tool sound-extract

This subcommand freezes the main CPU after boot. It then injects sound packets through the mailbox and records the selected audio backend. Source: `tools/commands/sound_extract.cpp`. Reference audio defaults to the oracle sound driver; native sound needs the generated sound program. Enhanced audio uses the ROM sequencer and PCM synthesizer without executing a sound CPU.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--rom-dir` | `DIR` | `F3RT_DEFAULT_ROM_DIR` if compiled in | ROM directory | Else the program stops. |
| `--set` | `NAME` | `landmakrj` | ROM set | |
| `--packet` | `HEX` | none | A packet to inject at the start of the event (time 0) | You can repeat the flag. White space in the string is ignored. The string must have an even length. Byte 0 must equal the packet size. The minimum size is 2 bytes. |
| `--at` | `SECONDS:HEX` | none | A packet to inject at a time after the event start | You can repeat the flag. The time must be 0 or more. All packets must be earlier than `--seconds`. |
| `--seconds` | `DURATION` | `5.0` | Time to advance the audio after the event start | Must be a positive number. |
| `--boot-frames` | `N` | `900` | Number of interpreted boot frames before the main CPU freezes | Must be a positive integer. The sound reset line must be released after boot. Else: `Sound CPU is still held in reset; increase --boot-frames`. |
| `--audio-backend` | `reference` or `enhanced` | `reference` | Choose emulated devices or approximate threaded Enhanced audio | `enhanced` is `landmakrj` only and rejects explicit `--sound-driver` and `--sound-trace`. |
| `--hle-events` | `FILE` | none | Save HLE voice start/release/stop/parameter events as CSV | Requires `--audio-backend enhanced`; not an F3SND2 CPU bus trace. |
| `--sound-trace` | `FILE` | none | Record a bus trace from cold boot | Reference audio only. |
| `--wav` | `FILE` | none | Write the audio as a 16-bit stereo WAV | |
| `--wav-window` | `full` or `event` | `full` | What the WAV contains | `full`: boot plus extraction. `event`: only the time after the event start. |
| `--sound-driver` | `oracle` or `native` | `oracle` | Sound CPU implementation for Reference audio | `enhanced` rejects explicit driver selection. |
| `--json` | none | off | Output machine-readable JSON result | |
| `--help`, `-h` | none | | Print usage | |

The program writes packets into the mailbox ring buffer at main address `0xc00000` (1024 bytes). It stops with `Mailbox full at scheduled tick; space commands farther apart` if there is no room. On success, it prints `SUCCESS backend=... driver=... set=... packets=...`.

```sh
./build/f3rt-tool sound-extract --sound-driver native --packet 038001 --seconds 3 --wav-window event --wav note.wav
```

## f3rt-tool gpu-compare

Strict-native Land Maker CPU-versus-GPU presentation diagnostics, built with
`F3RT_GPU`, SDL3 and generated main/sound programs. No interpolation in the
parity comparison. Native machine/audio output remains CPU-produced.

```sh
./build/f3rt-tool gpu-compare --seed 5 --frames 4000 --scale 4 --border 48 --layers
./build/f3rt-tool gpu-compare --seed 5 --frames 4000 --scale 4 --border 48 --bench
./build/f3rt-tool gpu-compare --seed 5 --frames 1560 --scale 4 --border 48 --interp fit --capture-frame 1500 --capture-frame 1560 --dump-dir /tmp/f3-water --bench
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--rom-dir DIR` | Configured ROM directory | Japan 2.01J ROM set |
| `--seed N` | `12345`, or `SEED` environment | Deterministic single-player input schedule |
| `--frames N` | `4000` | Native frames to execute |
| `--scale N`, `--border N` | `1`, `0` | Diagnostic GPU scale 1..8; border 0..160. Player numeric/auto scales remain capped at 4. |
| `--every N` | `1` | Sample every N frames; always sample the final frame |
| `--layers` | Off | Compare all nine isolated supported layers plus the composite |
| `--bench` | Off | Varied-scene timings after 600 frames; 100 serial/threaded/GPU repeats of the last supported frame |
| `--dump-dir DIR` | None | External CPU/GPU PNGs and native BMPs |
| `--interp off\|linear\|fit` | `off` | Second opt-in renderer; CPU parity comparison always uses the off renderer |
| `--capture-frame N` | None | Repeatable; forces selected off/mode PNGs and complete PF2 row CSVs, even off the sampling interval; needs `--dump-dir` |
| `--change-scale FRAME:SCALE` | None | Repeatable runtime transition; canonical constructor scale remains 1, each transition forces parity/capture and asserts unchanged snapshot bytes. Also changes scale during induced trail-history branches. |
| `--inject-frame N` | `1407` | Supported baseline for diagnostic branches |
| `--inject-bitmap`, `--inject-trails`, `--inject-globalflip` | Off | Induce actual unsupported-mode boundaries and verify oracle presentation/recovery |
| `--inject-sprite-boundaries` | Off | Inject sprite-RAM display lists to witness ROM-texel row order, flips, collapsed spans, overlap and edge clipping/cull |
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

## f3rt-tool motion

Strict-native two-machine temporal GPU proof, requiring generated main/sound
programs and `F3RT_GPU`. One seeded machine is repeatedly presented; the other
is not. Checks native pixels/audio/state parity, exact current-frame endpoints,
visible intermediate ROM motion, frozen-repeat stability, moving/static/rejected
native-pair census and discontinuity snapping. `f3rt-test-motion_interp` separately
exercises count shifts, identity ties, transforms, controls, wraps and bounds
without ROMs or an SDL window. Final-frame load/reexecution checks replay.
Even diagnostic scales also check exact horizontal/vertical half-native-pixel
text translations through the GPU using a captured game glyph.

```sh
./build/f3rt-tool motion --frames 4000 --seed 12345 --scale 3 --every 20 \
  --interp fit --dump-dir build/motion-evidence/fit
```

Flags: `--rom-dir DIR` (configured ROM path), `--frames N` (4000), `--seed N`
(12345), `--scale N` (1–8, default 1), `--every N` (1), `--interp off|linear|fit`
(`off`), `--dump-dir DIR`, `--demo`, `--demo-seconds N` (30, 1–3600), `--help`.
Frames/every must be positive. Proof runs without visible paired ROM midpoint
motion deliberately fail. PNG captures and fixed phase grids are not a
physical refresh-rate measurement.

`--demo` instead warms up the specified seeded ROM frames, freezes the machine,
and adds a bounded 2px/native-frame horizontal playfield pan to a host-only scene
copy. One window shows **left native / right interpolated** through the full GPU
shader path and one shared display tick/drawable. Periodic logs distinguish
mode Hz, requested/observed display-link Hz and accepted drawable submissions/s;
scanout is unknown. Escape exits. `--dump-dir` saves canonical/midpoint images
and one actual app-owned full-window composition submitted to the drawable.

```sh
./build/f3rt-tool motion --demo --frames 1560 --seed 5 --scale 3 --demo-seconds 30
```

## Runtime unit tests

The per-area runtime test binaries take no arguments and do not read ROM files. They build only when `BUILD_TESTING` is on. CTest registers each as `runtime-<area>`.

```sh
ctest --test-dir build -R runtime-
```

Each test runs under GoogleTest with RapidCheck property testing, reporting test pass/fail results.

## uv run python -m recomp

The recompiler runs from the repository root via `uv run`. Source: `recomp/__main__.py`. Dependencies are declared in `pyproject.toml` (`capstone==5.0.9`).

```sh
uv run python -m recomp discover --config games/landmakrj/config.toml --rom-dir roms/landmakrj --output build/landmakrj
uv run python -m recomp emit --config games/landmakrj/config.toml --rom-dir roms/landmakrj --output build/landmakrj
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
| `--rom-dir` | `DIR` | none | Directory with the sound ROM chips | Required. Loads and validates the selected config sound lanes, padding and mirroring; no `sound.bin` fallback. |
| `--config` | `TOML` | Japan config | Selected sound manifest and exclusions | Pass the matching game config explicitly for another title. |
| `--output` | `DIR` | none | Output directory | Required. |
| `--blocks-per-file` | `N` | `1024` | Number of block functions in each C file | |
| `--coverage` | `all_aligned` | `all_aligned` | Coverage mode | The only allowed value. The script compiles every even address of the sound ROM. |
| `--profile-tiers` | `PROFILE` | none | Split sound units into hot/cold compilation tiers | Full aligned table retained; mutually exclusive with slim. |
| `--profile-slim` | `PROFILE` | none | Emit only profiled sound statements and a sparse sorted table | Requires paired slim runtime; no interpreter fallback for omitted entries. |

```sh
uv run python tools/compile_sound.py --config games/landmakrj/config.toml --rom-dir roms/landmakrj --output build/generated/sound-landmakrj
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


## Python tools

All tools below run from the repository root with `uv run python`. See [Tools](/reference/tools) for a short description of each tool.

### f3 gameplay-seeds

This command runs `f3rt-tool gameplay` once for each seed. It fails if any seed fails.

| Flag | Argument | Default | Meaning | Notes |
| --- | --- | --- | --- | --- |
| `--binary` | `PATH` | `build/f3rt-tool` | Program to run | The script stops if the file does not exist. |
| `--rom-dir` | `DIR` | none | Passed on as `--rom-dir` | If omitted, the binary uses its built-in default. |
| `--seeds` | `N [N ...]` | `1 2 3 4 5 6 7 8` | Seeds to run | Each seed must fit in an unsigned 64-bit number. |
| `--frames` | `N` | `40000` | Frames for each seed | Must be positive. |
| `--dump-captures-dir` | `DIR` | none | Save `seed_N.bmp` for each seed | The script creates the directory. It passes `--surface` to the binary. |
| `--video-diff` | none | off | Turn on video comparison | |
| `--video-layer-mask` | `N` | `511` | Layer mask (accepts `0x` hex) | Must be 1 to 511. |
| `--video-diff-every` | `N` | `120` | Compare interval | Must be positive. |

Exit code: `1` if any seed failed, else `0`.

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

This script compares two 16-bit stereo WAV files with one fixed delay correction. It needs NumPy and SciPy, which are in the `analysis` dependency group of `pyproject.toml`. Run it with `uv run --group analysis python tools/compare_audio.py REFERENCE.wav CANDIDATE.wav`.

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

### tools/differential (run.py and `uv run python -m tools.differential`)

The differential harness compares the recompiler output for random 68020 instructions against the Musashi reference core. `uv run python tools/differential/run.py` and `uv run python -m tools.differential` run the same `main()` function.

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
| `SEED` | `f3rt-tool gameplay` | Seed, if `--seed` is absent |
| `MAME_BIN` | `tools/mame/run_capture.sh` | MAME executable |
| `CC`, `AR` | `tools/differential` | C compiler and archiver |
| `F3_CAPTURE_DIR`, `F3_CAPTURE_START`, `F3_CAPTURE_COUNT`, `F3_CAPTURE_STEP`, `F3_CAPTURE_EXIT` | `tools/mame/capture.lua` | Capture settings |
| `F3_AUDIO_TRACE` | `tools/mame/audio_trace.lua` | Audio trace output path |
