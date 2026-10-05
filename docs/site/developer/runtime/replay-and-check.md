# f3rt-replay and f3rt-check

The runtime provides two validation programs.
`f3rt-replay` compares isolated devices with recorded reference data.
`f3rt-check` exercises the board runtime with synthetic ROMs.

Both programs link the `f3rt` library. Neither needs generated game code. For the larger test setup, read the [Testing overview](/developer/testing/).

## f3rt-replay

The source is `runtime/replay.cpp`. CMake builds it always: `add_executable(f3rt-replay runtime/replay.cpp)`. It checks a single runtime part against a recorded reference, without the CPU. It has two modes. You must choose exactly one.

```text
f3rt-replay --rom-dir DIR (--captures DIR | --audio-trace FILE) --output PATH
```

| Option | Meaning |
| --- | --- |
| `--rom-dir DIR` | Directory with the extracted ROM files. Always needed. The tool loads the default set `landmakrj`. |
| `--captures DIR` | Video mode. A capture directory or a directory of capture frame directories. |
| `--audio-trace FILE` | Audio mode. An `F3AUD2` device-write trace. |
| `--output PATH` | Video mode: a directory for rendered frames. Audio mode: the WAV file. |

An unknown option, a missing value, or a wrong combination throws an error. The tool prints the message and returns 2.

### Video mode

The tool tests the FDP software renderer (`Video`) against MAME pictures. It does not run a CPU.

1. Load the ROMs and create a `Video`. Decode the graphics ROMs.
2. Find frame directories. If `DIR/graphics.bin` exists, the directory is one frame. Otherwise every sub-directory that contains `graphics.bin` is a frame. Sort them by name. If none are found, throw `No capture frames found`.
3. For each frame, read these files with `read_exact`:
   - `palette.bin` (32 KiB), `graphics.bin` (256 KiB), `control.bin` (32 bytes)
   - `spriteram_active.bin` (64 KiB)
   - `reference.argb` (320 by 232 by 4 bytes)
4. Call `video.reset()`, `video.set_active_spriteram(sprites)` and `video.render_frame(palette, graphics, control, pixels)`.
5. Write `rendered.argb` and `rendered.bmp` into `OUTPUT/<frame name>/`.
6. Compare each pixel with the reference on the three color channels. The alpha byte is ignored. Count the pixels that differ, the largest channel error and the mean channel error.
7. Print one line for each frame, then `frames=N total_mismatched_pixels=M`.

The exit code is 1 if any pixel differs and 0 if all pixels are equal. The expected result for the project captures is zero differences. `docs/developer/DECISIONS.md` records that the replay matched all 25 wide-attract captures exactly (1 856 000 of 1 856 000 pixels).

The capture files come from `tools/mame/capture.lua`. See [MAME comparison](/developer/testing/mame). Note that `spriteram_active.bin` and `reference.argb` come only from MAME. `dump_machine` in `capture_io.hpp` does not write them.

### Audio mode

The tool tests the sound chip emulation without a CPU. It plays the device writes that MAME made.

The trace file starts with the 8 bytes `F3AUD2` and two zeros. After the header, each record has 16 bytes. All numbers are little-endian.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | Time in main-CPU ticks. |
| 8 | 4 | Address. |
| 12 | 2 | Data. |
| 14 | 2 | Byte mask. |

The loop is:

1. Create an `Audio` object and load its sample ROM.
   No CPU runner executes the sound program.
   The command still loads the complete `RomSet`, so all expected chip files remain required.
2. For each record, advance the audio in steps of at most 16 000 ticks up to the record time. After each step, render the samples that are ready, and add them to the WAV file. Count frames and track the peak.
3. Stop at the record with address `0xffffffff`. It is the end marker. Time must not go backwards.
4. For address `0xfffffffe`, call `audio.reset_board()`. It is a whole-board reset record.
5. Otherwise, write the data to the audio bus. If the mask is `0xffff`, do a 16-bit write. If not, write the high byte when mask bit 8 to 15 is set, and the low byte when bit 0 to 7 is set.

If the file has no end marker, the tool throws `Truncated audio trace: missing end timestamp`. At the end it prints `audio_writes=... frames=... sample_rate=... peak=...`.

Advancement occurs before each record takes effect, including the reset and end markers.
The end marker supplies the final rendering timestamp.
Reset records and end records do not increment `audio_writes`.
The counter counts ordinary records, even when their byte mask selects no writes.
Successful audio replay returns 0; it does not compare the WAV against MAME itself.

::: info
`F3AUD2` is the MAME trace. `F3SND2` is the runtime trace from `SoundTrace`. The two formats are different. See [Support files](/developer/runtime/support#sound-trace).
:::

The script `tools/compare_audio.py` compares the WAV file with a MAME recording. `docs/developer/DECISIONS.md` records that the replay of a 62-second baseline gave a correlation of 0.999978 with MAME after a one-sample latency fix. Treat this as a device-level result. It does not prove that the integrated game audio matches.

## f3rt-check

The source is `runtime/check.cpp`. CMake builds it only when `BUILD_TESTING` is on, and registers it as the CTest test `runtime-devices`.

```sh
ctest --test-dir build -R runtime-devices
```

The program has no options. It runs all checks in order. On the first failed check it prints `FAIL <message>` and returns 1. On success it prints one `PASS` line and returns 0.

The helper `require(ok, why)` throws a `std::runtime_error` with the text `why`. So each message in the source states the rule that the check proves.

### The fixture

`fixture()` builds a `RomSet` with zero-filled regions of the right sizes. It then adds:

- The main ROM vectors: SSP `0x41fff0`, PC `0x100`.
- At `0x100`: `MOVEQ #42,D0` and a `BRA` to itself.
- A sample word in the sample ROM to test a 20-bit address.

The checks need no game data, so they run anywhere.

### What the checks cover

| Area | Checks |
| --- | --- |
| Game video | Tile descriptors, sprite descriptors, the sprite top edge. |
| Audio mixer | Mixing, 10 seconds of audio at the advertised rate with no drift, board reset keeps queued audio and the clock phase. |
| Main and sound ordering | Reset release and mailbox accesses at 1, 2 and 4 bytes (also unaligned) see only earlier sound work. A reset instruction keeps earlier sound work. |
| Sound time partitioning | A real sound program counts foreground loops while a timer IRQ fires 10 times. The state is equal for main-time steps of 1, 7, 64, 511 and 4096. |
| Raster events | Cold reset takes 4 cycles and sets PC to `0x100`. The first vblank waits one full frame. IRQ2 and IRQ3 times, 10 000 cycle delay, next frame deadlines. |
| Instruction timing | `MOVEM` load and store costs, rotate costs, `TRAP #n` costs for 16 vectors (native `f3_exception` and the reference core), sound 68000 costs. |
| Sound IRQ | 44-cycle entry for four vectors, STOP wake with 52 cycles for ten budgets. |
| DUART | Transmitter ready and empty states for both channels, counter restart, mode and reset rules. |
| Memory | Big-endian misaligned access across the work RAM mirror, ROM is read-only, shared RAM byte lanes in both directions. |
| Sound memory | Stopped voice reads a 20-bit sample address. CPU reset keeps the work RAM. |
| Coins and input | A coin counter counts rising edges only. A pressed start key reads as 0. |
| EEPROM | The checks listed on [Input and EEPROM](/developer/runtime/input-and-eeprom#tests). |
| Deadline | A runnable boundary publishes a future deadline. Raising the mask keeps it. Lowering it clears it. A watchdog strobe does not hide a pending recheck. |
| IRQ entry | An IRQ breaks STOP, sets the mask, stacks a 68020 format frame, and requests a fresh lookup. |
| Exceptions | Divide by zero costs 38 cycles and uses a format-2 frame. A real RTE restores the resume PC. |
| Interpreter | SR lowering by the interpreter clears the deadline. Reset keeps native D and CCR and costs 4 cycles once. |
| Dispatch | A valid block table is accepted. Duplicate PCs are rejected. A native block runs. Fallback executes one real instruction. Strict mode rejects fallback and reports the PC. |
| Sound reset | A reset write keeps DSP and DUART state. |
| Watchdog | Expiry can come before the next raster IRQ. STOP advances to the expiry. The reset holds the sound CPU, restores the DUART vector, keeps work RAM, reloads vectors and clears DSP registers. |

### Example: the deadline check

The check shows how the code tests the ABI directly:

```cpp
auto &cpu = m->cpu;
cpu.usp = 0x400800; cpu.a[7] = 0x401000; cpu.sr = 0x2600;
require(m->boundary() == 0 && cpu.dispatch_deadline > cpu.cycles,
        "Runnable boundary publishes a future deadline");
const auto masked_deadline = cpu.dispatch_deadline;
f3_set_sr(&cpu, 0x2715);   // raise the mask
require(cpu.dispatch_deadline == masked_deadline,
        "Raising the IRQ mask preserves the event deadline");
f3_set_sr(&cpu, 0);        // lower the mask
require(cpu.dispatch_deadline == 0,
        "Lowering the IRQ mask invalidates the cached deadline");
```

The same style applies to every check. Set up the state, call the public function, and compare the visible result. Write new runtime checks this way. See [Contributing](/developer/contributing).

## Key points

- `f3rt-replay` tests the renderer and the sound chips against MAME data. It uses no CPU.
- `f3rt-check` is the unit test of the runtime core. It uses a tiny synthetic ROM.
- Both programs print results that are easy to compare in scripts.

## Sources

- [Replay command and comparison loop](https://github.com/ansxor/f3-recomp/blob/main/runtime/replay.cpp)
- [Device checks and synthetic ROM fixture](https://github.com/ansxor/f3-recomp/blob/main/runtime/check.cpp)
- [Exact-size capture reads and WAV output](https://github.com/ansxor/f3-recomp/blob/main/runtime/capture_io.hpp)
- [Validation build targets](https://github.com/ansxor/f3-recomp/blob/main/CMakeLists.txt)
