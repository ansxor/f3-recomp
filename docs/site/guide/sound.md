# Sound

This page explains sound generation and driver selection. It shows how to record WAV files and sound traces.

## Host audio backend and volume

There are two audio backends, selected with `--audio-backend enhanced|reference`:

- **Enhanced** uses separate host synthesis. It is the default for Land Maker Japan (`landmakrj`), the only set that supports it. It is not PCM-equivalent to Reference audio.
- **Reference** uses the native sound driver (or the oracle) and emulated chips. It is the default for every other set, and `--audio-backend reference` selects it for Land Maker Japan.

F1 → Audio lists the available choices as **Sound backend (restart)**; a backend the current set cannot use is greyed out. The choice is a restart-required control. Use **Save preferences** to store it explicitly (settings key `audio=enhanced|reference`); an explicit `--audio-backend` overrides the saved value. A saved Enhanced preference falls back to Reference for a set that does not support it, while an explicit `--audio-backend enhanced` for such a set is an error. See [Enhanced audio design and limits](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/HLE-AUDIO.md).

`--volume 0..100` (default 100%) and F1 volume affect host output only, not emulated gain or snapshots. Driver/oracle diagnostics below describe the Reference simulation path.


## How the sound works

The F3 board has a separate sound computer. It has these parts:

- A 68000 CPU that runs the **sound driver**. The driver is a program in the sound ROM (`e61-14.32` and `e61-15.33`).
- An ES5505 chip (called OTIS) that plays sampled sounds. It reads the sample ROM files.
- An ES5510 chip that adds effects.
- A 68681 DUART chip that gives the sound CPU its timer interrupts.
- An MB87078 chip that controls the volume.

The main CPU sends commands to the sound CPU through a mailbox in shared memory. The sound driver reads the commands and programs the ES5505 voices.

`--audio-backend reference` uses MAME-derived sound-device implementations, validated against reference output rather than physical hardware. It has two ways to execute the sound ROM; **native sound is not HLE**:

| Driver | Name in the option | What it is |
| --- | --- | --- |
| Native | `native` | The sound ROM, recompiled to C at build time by `tools/compile_sound.py`. This is the default when the build made it. |
| Oracle | `oracle` | The same ROM, run by the Musashi 68k interpreter. This is the reference. |

Both drivers write to the same emulated chips. The chips and the SDL3 audio output do not change.

```mermaid
flowchart LR
    M["Main CPU (generated code)"] -->|"mailbox commands"| S{"Sound driver"}
    S -->|native| N["Recompiled C blocks"]
    S -->|oracle| O["Musashi interpreter"]
    N --> C["ES5505, ES5510, DUART, MB87078"]
    O --> C
    C --> A["Stereo samples"]
    A --> SDL["SDL3 audio"]
    A --> WAV["WAV file (optional)"]
```

## Enhanced audio

`--audio-backend enhanced` selects approximate audio on a separate 48 kHz worker.
It reads Land Maker's music/instrument data and sample ROM directly, without
executing the sound CPU or ES5505/ES5510 programs. Do not combine it with
`--sound-trace`, and do not give it a `--sound-driver` (a tool option, described below).
It is available only for `landmakrj`.

Explicit `--audio-backend` selections override the saved backend.
See [Enhanced audio evidence and limits](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/HLE-AUDIO.md).

## Choose the sound driver

The sound driver matters only with Reference audio. `landmakr` and `f3rt-run` have no `--sound-driver` option; they choose the driver themselves. The `f3rt-tool` subcommands `gameplay` and `sound-extract` accept `--sound-driver native` or `--sound-driver oracle`.

| Situation | Driver |
| --- | --- |
| You run `landmakr` or `f3rt-run` with Reference audio and built with `F3_ROM_DIR` (generated sound code) | `native` |
| You run `landmakr` or `f3rt-run` with Reference audio and the program has no generated sound code. This happens if you configured the build without `F3_ROM_DIR`. | `oracle` |
| You run one of the tools and give no option | `oracle` |
| You give a tool `--sound-driver oracle` | `oracle` |
| You give a tool `--sound-driver native` and it has no generated sound code | Error: `Native sound requires a generated sound program (F3_ROM_DIR)` |
| You give a tool another value | Error: `--sound-driver must be oracle or native` |

Finite seeded runs compare native sound bus traces and WAV output with the oracle. These checks do not establish correctness for every reachable game state. See [Developer evidence](/developer/evidence) and the [sound-driver document](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/SOUND-DRIVER.md) for coverage and limits.

::: info Why two drivers
Use the oracle to investigate a suspected native sound CPU bug. Use the native driver for normal Reference play. Both execute the same ROM and feed the same emulated sound devices.
:::

The native driver needs exactly the sound ROM with CRC32 `5a7e9117`. For another ROM the program stops with `SoundNative: unsupported sound ROM CRC 0x... (expected 0x5a7e9117)`.

## Play without sound

Add `--no-audio` to skip the sound device. The machine still makes the samples. A `--wav` file still has the full audio. Headless runs never open a sound device.

## Record a WAV file

Use `--wav FILE` to record 16-bit stereo PCM. Reference audio uses the ES5505 audio core's sample rate; Enhanced audio uses 48 kHz.

```sh
./build/landmakr --headless --frames 3600 --wav build/audio.wav
```

The program fills in the WAV header at the end of the run. Do not read the file while the program runs.

::: tip The first seconds are quiet
The game sets its output gain at about 13.23 seconds (780 frames at the native frame rate). A recording that stops earlier can be very quiet or silent.
:::

Keep WAV files in the ignored `build/` directory. They contain audio from the ROM.

## Record a sound trace

A sound trace records every read and write that the sound CPU makes on its bus, and every write from the main CPU to the sound mailbox. The trace does not change the timing and does not read any device register a second time. It needs Reference audio, because Enhanced audio runs no sound CPU.

```sh
./build/landmakr --headless --frames 6000 --audio-backend reference --sound-trace build/run.sound --wav build/run.wav
```

Notes:

- The trace file starts with the 8 bytes `F3SND2` and two zero bytes. Each record has 32 bytes.
- The trace contains data from the ROM. Keep it in `build/`. Do not commit it.

Decode the trace with the Python tools. All paths are examples.

```sh
python3 tools/decode_sound.py build/run.sound --output build/run-writes.jsonl.gz
python3 tools/decode_sound.py build/run.sound --notes-only --output build/run-notes.jsonl
```

| Option of `decode_sound.py` | Effect |
| --- | --- |
| `--output FILE` | Required. Writes JSON lines. A name that ends in `.gz` gives a compressed file. |
| `--notes-only` | Keeps only voice starts, commands and reset or end events. |
| `--commands-only` | Keeps only the submitted and consumed command packets. |

To compare the traces of the two drivers, record one trace with each driver (with `f3rt-tool sound-extract` or `f3rt-tool gameplay`, which take `--sound-driver`) and run the comparison tool.

```sh
uv run f3 compare trace build/oracle.sound build/native.sound
```

The tool needs exact records, timestamps and ownership data. Add `--json FILE` to save the result.

## Extract one sound or one music sequence

The `f3rt-tool sound-extract` subcommand boots the game, freezes the main CPU, and then sends sound commands that you choose. It writes a WAV file and a trace of only that sound.

```sh
cmake --build build --target f3rt-tool
build/f3rt-tool sound-extract --rom-dir /path/to/roms/landmakr \
  --sound-driver native --packet 038108 --packet 04860874 --seconds 5 \
  --wav-window event --sound-trace build/music.sound --wav build/music.wav
```

| Option | Default | Effect |
| --- | --- | --- |
| `--rom-dir DIR` | Build-time ROM directory, if any | ROM directory. |
| `--set SET` | `landmakrj` | ROM set. |
| `--packet HEX` | none | Send a packet at time zero of the event. You can repeat the option. |
| `--at SECONDS:HEX` | none | Send a packet at the given time. You can repeat the option. |
| `--seconds N` | 5.0 | How long to run after the event origin. Must be greater than 0. |
| `--boot-frames N` | 900 | Frames to boot before the program freezes the main CPU. Must be greater than 0. |
| `--sound-trace FILE` | none | Write a trace from cold boot. |
| `--wav FILE` | none | Write a WAV file. |
| `--wav-window full\|event` | `full` | `full` records from cold boot. `event` records from the event time. |
| `--sound-driver oracle\|native` | `oracle` | Driver to use. |
| `--audio-backend reference\|enhanced` | `reference` | Choose board emulation or the independent Enhanced engine. Enhanced is `landmakrj` only and rejects `--sound-driver` and `--sound-trace`. |
| `--hle-events FILE` | none | HLE voice-event CSV; requires `--audio-backend enhanced`. |

A packet is a hex string. The first byte is the total packet size, including the size byte and the opcode byte. For example, `038001` has size 3, opcode `0x80` and parameter `0x01`. The program adds no hidden setup packets.

The default of 900 boot frames is after the output-gain writes of the game. A smaller number can leave the sound attenuated. For packet meanings and timing, read the [sound documentation](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/SOUND-DRIVER.md).

## Next steps

- The sound compiler: [Developer: sound compiler](/developer/recompiler/sound-compiler).
