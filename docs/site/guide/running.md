# Controls and options

This page explains the keys and saved settings. It also covers headless runs, output files and the final summary.

For a table of every option, read the [command-line reference](/reference/cli). This page groups the options by task.

## Keys

The window must have focus. These are the default keyboard bindings; F1 lets you remap both offline player profiles.

| Key | Action |
| --- | --- |
| Arrow keys | Move |
| `Z` | Button 1 |
| `X` | Button 2 |
| `C` | Button 3 |
| `5` | Coin slot 1 |
| `6` | Coin slot 2 |
| `1` | Start 1 |
| `2` | Start 2 |
| `F3` | P1 service switch |
| `F2` | P1 test switch |
| `F1` | Open/close the menu |
| `F12` | Save a PNG screenshot |
| `Escape` | Close the menu, or quit when it is closed |
| `F11` or `Alt+Enter` | Toggle fullscreen |

Each key acts as a switch. The input is active while you hold the key. Release the key to release the switch.

When the window loses focus, the program releases all keys. It does this so that no key stays pressed.

P2 has only `2` (start) and `6` (coin) by default. Bind its directions/buttons in F1 for offline two-player play. There are no default gamepad bindings; select the device and capture buttons or axis directions in the menu.

In [online play](/guide/netplay), your local P1 profile controls your assigned player slot, including start and coin.

## F1 menu, preferences and slots

The menu pauses solo play. Network simulation continues while it is open, with local P1 input neutral. Closing the menu or losing focus releases captured/held input; release a held control before pressing it again.

Use **Save preferences** explicitly to persist settings and remaps. Preferences load before CLI arguments, so explicit flags win. `--config FILE` selects a different preferences file; the default is `settings.cfg` under `SDL_GetPrefPath("f3-recomp", "f3rt")`. `states/` and `screenshots/` live alongside it. Volume is host output only (`--volume 0..100`, default 100%).

The menu offers save/load slots **0–9** for offline strict-native play. A slot requires a compatible build, ROMs and presentation geometry. Slots are unavailable during network play; they are not portable canonical netplay snapshots. F12 writes a PNG into `screenshots/`; GPU postprocess captures include the effect, not the menu.

Only controls labeled live by the UI take effect immediately. Respect restart-required labels rather than assuming every saved preference reconfigures the running machine.

## The window

The window opens at 960 by 696 pixels and you can resize it. The picture keeps its shape. The program adds black bars if the window has a different shape. The size 960 is 320 native columns times 3. With a video border the window is wider.

The window title is `f3rt — landmakrj`. In online play the title shows the match state instead.

On start the program prints one line that starts with `window_open`. It gives the video driver, the renderer name, the video mode, the internal size and the filter.

## Settings and the EEPROM

The arcade board stores the game settings in a small EEPROM chip (a 93C46 with 64 words of 16 bits).

By default the program does **not** keep the EEPROM. Each start begins with an erased chip, and the game writes its factory defaults again.

Use `--eeprom FILE` to keep the settings between runs.

```sh
./build/landmakr --eeprom build/landmakr.nv
```

| Situation | Result |
| --- | --- |
| The file does not exist | The program starts with an erased EEPROM. |
| The file exists and has exactly 128 bytes | The program loads it. |
| The file exists and has another size | The program stops with `Invalid EEPROM file: PATH`. |
| The run ends normally | The program writes the file. |
| The run stops with an error | The program does not write the file. |

A normal end is the `Escape` key, a closed window, or the end of a `--frames` limit.

The file has 128 bytes. It stores the 64 words as big-endian 16-bit values. A new file is all `0xff` until the game writes to it.

In netplay, the host's canonical handoff state becomes the match state. Independent EEPROM files and solo histories are supported; EEPROM equality is not a join requirement.

## Fast boot

A window launch runs the power-on self-test and boot waits at full speed by default, without drawing the frames and with the sound samples discarded. The machine executes the same frames as an ordinary power-on. Fast boot is host-side only: it does not patch the ROM, change the CPU start, or write game RAM.

Fast boot does not alter emulation: with the same build, ROMs, EEPROM contents and inputs, `frame_crc` is the same with fast boot on or off. Because fast boot also loads the initialised EEPROM below, compare against an ordinary run given that file with `--eeprom`. Builds whose game provides no boot signal never fast boot.

| Option | Default | Effect |
| --- | --- | --- |
| `--fast-boot on\|off` | `on` for a window | Run the boot frames without presentation. `--fast-boot off` restores the ordinary throttled boot. |
| `--boot-cache on\|off` | `off` | Also store the post-boot state and skip the boot frames on later launches. |

The F1 menu adds **Fast boot** and **Boot state cache** in the Save states panel. They take effect on the next launch and are written only by **Save preferences**. Explicit CLI flags override the saved values.

Fast boot is used only when nothing needs the per-frame output. A headless run ignores it unless you pass `--fast-boot on` explicitly, and it is disabled whenever `--wav`, `--dump-dir`, `--sound-trace`, `--profile-out` or `--fallback-report` is given. Online play always runs the ordinary boot.

Fast boot prints one `fast_boot frames=N ms=T source=turbo|cache eeprom=generated|loaded|none` line before the summary.

### Initialised EEPROM

When fast boot is active and no `--eeprom` file is given, the program keeps an initialised 128-byte EEPROM at `eeprom/landmakrj-init.bin` beside the config file (the same base as `states/`). It records the game's own factory defaults, captured when boot completes and before any input. The first fast-boot launch that reaches boot completion writes it; a launch that quits or stops earlier writes nothing. Later launches load it, which skips the game's blank-EEPROM initialisation reboot (about 300 frames), so boot completes at frame 478 instead of 781.

### Boot state cache

With **Boot state cache** on (or `--boot-cache on`), the program also stores the post-boot machine state at `boot/landmakrj.state` beside the config file. A later launch loads it and skips the boot frames entirely. The cache is accepted only when the build, ROMs, state format and presentation geometry match; otherwise it is ignored, the boot runs normally, and the cache is rewritten. The state is captured at boot completion, before any input, so it is deterministic.

## Service and test mode

`F3` presses service and `F2` presses test in the default P1 profile. F1 is the frontend menu, not the game's service switch. The game defines the effect of service and test.

## Run without a window

Use `--headless` to run without a window, keyboard or sound device. You must also give `--frames N`. The program stops after N frames.

```sh
./build/landmakr --headless --frames 600
```

Without `--frames` the program stops with `Headless execution requires --frames`.

A headless run without netplay does not wait. It runs as fast as the computer allows. The machine does the same work as in a window, so the result is the same. An online match keeps the frame pace unless you add `--unthrottled`.

| Option | Effect |
| --- | --- |
| `--frames N` | Stop after N frames. Also works with a window. |
| `--headless` | Do not open a window. Needs `--frames`. |
| `--no-audio` | Do not open the sound device. The machine still creates the sound samples. |
| `--unthrottled` | With a window, do not wait between frames. The game runs faster than real time. |
| `--wav FILE` | Write all sound samples to a 16-bit stereo WAV file. |
| `--surface FILE` | With a window and `--frames`, save a CPU window BMP or GPU internal-resolution PNG after the last frame. |

The runtime uses MAME-derived frame timing: a 6,671,500 Hz pixel clock and 432×262 total raster, about 58.94 frames per second. It does not substitute a 60 Hz clock or claim physically measured board timing.

## Save frame data for tests

These options write machine state. They are for developers and for comparison with MAME.

| Option | Effect |
| --- | --- |
| `--dump-dir DIR` | Write one folder per dumped frame, named `frame_0001` and so on. |
| `--dump-start N` | First frame to dump. Default is 1. |
| `--dump-every N` | Dump every Nth frame. Default is 1. Must be greater than 0. |

Each folder holds `palette.bin`, `graphics.bin`, `control.bin`, `mainram.bin`, `shared.bin`, `rendered.argb`, `rendered.bmp` and `cpu.json`. The files can contain ROM-derived data. Keep them in an ignored folder such as `build/`.

These dumps are not complete inputs for `f3rt-replay`. Replay also needs the active sprite RAM and a MAME reference image. See [Replay options](/reference/cli#f3rt-replay).

## The summary line

At the end of every run the program prints a summary beginning with `set=`. Values depend on the build, inputs and run length; they are diagnostics, not a universal expected-output checkpoint.

| Field | Meaning |
| --- | --- |
| `set` | The ROM set. Always `landmakrj` for `landmakr`. |
| `frames` | Number of frames that the machine ran. |
| `pc` | Main CPU program counter at the end. |
| `sound_pc` | Sound CPU program counter at the end. |
| `sound_driver` | `native` or `oracle`. See [Sound](/guide/sound). |
| `frame_crc` | CRC32 of the last native 320 by 232 picture. |
| `cycles` | Main CPU cycles that the machine ran. |
| `native_blocks` | Number of generated code blocks that ran. |
| `fallback_instructions` | Number of instructions that the diagnostic interpreter ran. Must be 0 for the `landmakr` program. |
| `audio_frames` | Number of stereo sample pairs that the machine made. |
| `audio_peak` | Largest absolute sample value. |
| `nonzero_samples` | Number of samples that are not zero. |

The `frame_crc` value is useful to check that two runs give the same picture. Two runs with the same ROM, build and inputs give the same value.

Before this line the program can print a `fast_boot` line (see [Fast boot](#fast-boot)) and `VIDEO` lines. The `VIDEO` lines describe the game-data renderer. See [Video and presentation](/guide/video#the-video-report). In online play the program also prints a `netplay_confirmed` line. See [Online play](/guide/netplay).

## Choose the ROM set and directory

| Option | Effect |
| --- | --- |
| `--rom-dir DIR` | Load the ROM files from DIR. The `landmakr` program uses the directory from the build as default. |
| `--set landmakrj` | Select the ROM set. This is the default. The `landmakr` program accepts only this value. |

If the program knows no ROM directory, it stops with `--rom-dir required; --dump-every must be positive`. This happens when you run `f3rt-run` without `--rom-dir`.

## Diagnostic modes

These options exist for developers. The normal game does not need them.

| Option | Effect |
| --- | --- |
| `--translated` | Use the generated main-CPU code. This is already the default in `landmakr`. |
| `--allow-fallback` | Let the diagnostic interpreter run any instruction that the generated code does not cover. |
| `--fallback-report FILE` | Write a tab-separated list of the program counters that used the interpreter, with counts. |

::: warning Fallback is not the native game
With `--allow-fallback` the `landmakr` program changes its default video mode to `fdp`. A run with fallback does not count as proof that the native game works. Without this option, the `landmakr` program stops at the first instruction that the generated code does not cover. It reports `Untranslated main CPU instruction at PC 0x...`.
:::

## Other programs: `f3rt-run`

The `f3rt-run` program uses the same source file as `landmakr`. It has different defaults.

| Item | `landmakr` | `f3rt-run` |
| --- | --- | --- |
| ROM directory | Set at build time | You must give `--rom-dir` |
| Main CPU | Generated code | Interpreter (add `--translated` for generated code) |
| Fallback | Off | On |
| Video | `game` | `fdp` |
| Sound driver | `native` if the build made it | `native` if the build made it, else `oracle` |

## Next steps

- Picture options: [Video and presentation](/guide/video).
- Sound options: [Sound](/guide/sound).
- All options in one table: [command-line reference](/reference/cli).
