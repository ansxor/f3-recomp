# Unit checks

This page explains the tests that need no ROM or MAME.
It covers the split C++ runtime tests, the Python tests and the Go tests.
You will learn what each test checks and how to add a check.

These tests check isolated behavior. Run the relevant tests before the whole-game checks.

| Test set | Language | Command | Needs ROM |
| --- | --- | --- | --- |
| `f3rt-test-<area>` (CTest name `runtime-<area>`) | C++ | `ctest --test-dir build -R runtime-` | No |
| `tools/test_*.py` | Python | `python3 -m unittest discover` | No |
| `netplay/server/*_test.go` | Go | `go test -race ./...` | No |
| Differential harness | Python and C | See [Differential testing](/developer/testing/differential) | No |

## C++ runtime tests

The C++ checks are split by area under `runtime/tests/`. CMake builds the `f3rt-test-support` static library and one executable per area (`f3rt-test-<area>`), registering each as `runtime-<area>` with CTest when `BUILD_TESTING` is on.

```sh
cmake -S . -B build -G Ninja -DF3RT_SDL=OFF -DCMAKE_BUILD_TYPE=Release
ctest --test-dir build -R runtime-
```

Each area test prints `PASS` on success or `FAIL <message>` on failure. Shared fixture and assertion helpers are in `runtime/tests/support.hpp` and `support.cpp`. The fixture uses zero-filled ROM data, an initial stack at `0x41fff0`, a small loop at `0x100`, and one sample word.

| Area | Source | Coverage |
| --- | --- | --- |
| video | [`runtime/tests/video.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/video.cpp) | FDP geometry and game-video decoding checks. |
| input | [`runtime/tests/input.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/input.cpp) | Local and dial inputs, scripts, coin edges and active-low start. |
| eeprom | [`runtime/tests/eeprom.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/eeprom.cpp) | EEPROM protocol and factory image. |
| cpu | [`runtime/tests/cpu.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/cpu.cpp) | CPU dispatch, memory, timing, exceptions, IRQ and watchdog. |
| sprite_units | [`runtime/tests/sprite_units.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/sprite_units.cpp) | Sprite unit sandbox. |
| audio | [`runtime/tests/audio.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/tests/audio.cpp) | Sound ordering, audio timing, DUART, DSP, mixer and reset behavior. |

The checks use a synthetic fixture and do not need game ROMs.

### What the checks cover

| Function | Component | What it proves |
| --- | --- | --- |
| `check_game_tile_observation` | `GameTiles` | Raw video-RAM cells decode palette base, pen mask and blend; flip X/Y mirror the sampled texel; a masked extra plane is cleared by the palette's low bit. |
| `check_game_tile_row_sampling` | `GameTiles` | Wrapping, repeats, reverse X jumps and global screen flip sample the correct raw cell across all four layers. |
| `check_game_sprite_descriptors` | `GameSprites` | Scaled grids keep every tile descriptor. Submitted sprites appear only after the next latch. An unreadable descriptor is unsupported. |
| `check_game_sprite_top_edge` | `GameSprites` raster | A fully clipped scaled sprite does not leak its last row into the viewport. |
| `check_audio_mixer` | `Audio` | Board gain, signed PCM scaling, channel mute, the two gain stages, and what a CPU-line reset or a board reset keeps. |
| `check_main_sound_ordering` | `Machine` and `Audio` | Mailbox writes and reads at widths 1, 2 and 4 see sound execution in the right order. A reset release cannot run sound code in the past. |
| `check_audio_partitioning` | `Audio` | Sound IRQs and CPU state are the same for main-block partitions of 1, 7, 64, 511 and 4096 ticks. |
| Ten-second clock check in `main()` | `Audio` | Ten seconds of audio give exactly `sample_rate * 10` samples. There is no clock drift. |
| Raster and IRQ checks in `main()` | `Machine::boundary` | The first vblank comes one full frame after the VBSTART epoch. IRQ2 is raised at the deadline. IRQ3 follows after 10000 cycles. Deadlines stay on the absolute frame grid. |
| `check_movem` | Interpreter | EC020 MOVEM costs 3 cycles per stored register and 4 per loaded register. Width, sign extension and pre-decrement work. |
| `check_rotate_cycles` | Interpreter | Register shifts and rotates have no count surcharge. Immediate counts 1 and 8 have equal timing. |
| `check_trap_cycles` | `f3_exception` and the interpreter | `TRAP #n` takes 24 cycles for all 16 vectors and stacks a format-0 frame. |
| `check_sound_cycles` | Sound CPU (68000) | Cycle costs of ADDQ, address arithmetic, immediate arithmetic, TAS, bit operations, DIVU.W and MULS.W for several operands. The DIVU and MULS tables follow operand-dependent timing. |
| `check_sound_irq` | Sound CPU | IRQ entry costs 44 cycles. A STOP that unmasks a pending IRQ costs 52 cycles in total, for ten different cycle budgets from 1 to 52. |
| `check_duart_tx` | `MC68681` | Transmit timing, holding register overflow and ready interrupts on both channels. |
| `check_duart_counter` | `MC68681` | Counter restart, reload, mode and reset behavior. |
| Memory checks in `main()` | `Machine` | Work RAM mirror and wrap, ROM is read-only, shared RAM byte lanes, OTIS stopped-voice readback, coin edge detection, active-low start input. |
| EEPROM checks in `main()` | `Eeprom` | 93C46 protocol: write protect at power-on, EWEN, EWDS, busy times (write 1750 us, erase 1000 us, erase-all and write-all 8000 us), sequential read wrap, power reset. |
| CPU ABI checks in `main()` | `f3_*` functions | Stack switch on SR change. The deadline cache is invalidated when the interrupt mask goes down. IRQ entry makes a 68020 frame. Divide-by-zero charges 38 cycles. Native dispatch, fallback (exactly one real instruction), duplicate block rejection, warm reset (4 cycles, once). |
| Watchdog checks in `main()` | `Machine` | The watchdog expires after 3 seconds of main clock. STOP advances to the expiry and cannot skip it. A whole-board reset keeps sound work RAM and reloads boot vectors, and clears the DSP registers. |

Some checks run the same input on the interpreter (`m->interpreter->run_main(1)`) and on the ABI function (`f3_exception`). They require both to agree. This protects the rule that the native path and the reference path have the same timing.

`docs/developer/DECISIONS.md` records that several of these checks failed before the fix that they protect, and pass after it. Examples are the pending-IRQ-at-STOP check and the DC-voice sound check.

### What the runtime tests does not prove

It tests single rules with small inputs. It does not run the game. It does not replace the gameplay gates. See [Machine, memory and scheduling](/developer/runtime/machine).

### How to add a check

1. Find the function whose component you changed. Add a `require()` call at the end, or add a new function.
2. Write the message as a rule: "X does Y when Z".
3. Call the new function from `main()`, before the code that builds the main `Machine` if it needs its own machine.
4. Make the check fail first. Revert your fix, run the runtime tests, and see the `FAIL` message. Then restore the fix.

Use the fixture for a machine. If you need sound code, write it with `m->audio->write16()` into sound RAM, as `check_main_sound_ordering` does.

## Python unit tests

Three files in `tools/` hold the Python tests. They use `unittest`. They need Capstone 5.0.9 and, for one file, a C compiler. They use synthetic data only.

```sh
PYTHONPATH=build/python python3 -m unittest discover -s tools -p 'test_*.py'
```

Run the command from the repository root. The `recomp` package must be importable. A run of this command on the documented source ran 18 tests and passed.

### `test_discovery.py` (11 tests)

The tests build a small synthetic ROM (0x1000 bytes with the vectors SSP `0x410000` and PC `0x400`) and call `recomp.discovery.discover()`. They check which instruction addresses the discovery finds. See [Discovery](/developer/recompiler/discovery).

`ActorDiscoveryTests` check the recursive mode with actor scripts:

- A long callback address and cyclic script calls are followed, and script data is not decoded as code.
- Register-staged script records (`MOVE.L table(PC,Xn),An` followed by a store) give the callbacks, and the pointer-table strides are respected.
- A pointer table with an explicit `count` does not run into the data after it.
- Jump tables with backward destinations, staged through a register or `LEA`, are found.
- A signed full-format extension displacement locates a table correctly.

`AllAlignedDiscoveryTests` check the `all_aligned` coverage mode that the Japan set uses:

- A computed target with no pointer literal in the ROM is still found.
- A 24-bit pointer at an odd byte offset is found.
- A routine of more than 32 straight-line instructions is found without a filter.
- Instruction starts inside the extension words of another instruction are kept (overlapping starts).
- Odd PCs are never decoded. A truncated instruction at the end of the ROM is rejected and recorded in `invalid_pcs`. Odd entry points raise `ValueError`.
- The `recursive` mode still ignores code that nothing reaches.

### `test_generate.py` (2 tests)

These tests call `recomp.generate.generate()` on a tiny program. They write a driver in C, compile it with the C compiler (`CC` or `cc`, with `-std=c11 -O2 -Wall -Wextra -Werror`) and run it. They check the deadline behavior of the generated blocks:

- A block yields at the first instruction boundary that reaches `dispatch_deadline`, with flags written to SR (`cc_op == 0`) and the PC and cycles correct. It resumes inside the block and finishes with the right registers and flags.
- Overlapping entries skip extension words. A deadline stop and a restart inside the overlap give the right result.

See [Flags, timing and deadlines](/developer/recompiler/flags-and-timing).

### `test_decode_sound.py` (5 tests)

These tests check `decode_sound.py` with synthetic records. See [Sound traces and sound tools](/developer/testing/sound-tools).

## Go tests for the relay server

The Go tests live in `netplay/server/`. They need Go 1.22 or newer and nothing else. The server uses only the standard library.

```sh
(cd netplay/server && go test -race ./...)
```

| File | Tests |
| --- | --- |
| `protocol_test.go` | Invalid header, game-data wire compatibility, malformed game data, input count limit. |
| `impairment_test.go` | Delay, loss and duplication in the impairment layer, and deadline ordering in the heap. |
| `server_test.go` | Room match and data flow, identity mismatch (every field), room capacity and slot conflict, recovery of a lost completion verdict after one peer leaves, stale leave after a new session, endpoint spoofing, protocol mismatch, nonce migration. |

See [Relay server](/developer/netplay/server) and [Wire protocol](/developer/netplay/protocol) for the rules that these tests protect. A previous passing run does not establish the result on a changed build.

## Where the other tests are

| Test | Page |
| --- | --- |
| Instruction-level differential harness | [Differential testing](/developer/testing/differential) |
| Seeded gameplay regression | [Seeded gameplay regression](/developer/testing/gameplay-regression) |
| Netplay oracle | [Netplay oracle](/developer/testing/netplay-oracle) |

## Limits

- No automatic system runs these tests. The repository has only a documentation workflow. Run them before you commit.
- the runtime tests stops at the first failure. It does not list all failing checks.
