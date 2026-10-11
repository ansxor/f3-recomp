# Glossary

**What you will learn:** the meaning of the technical words in this project. The list is in alphabetical order. Each entry has one short definition. Entries link to the page that explains the topic.

## 0-9 and symbols

**68000** — A Motorola 16-bit CPU. The F3 sound board uses it. The sound driver runs on it. The reference core runs it in Musashi mode `M68K_CPU_TYPE_68000`. See [Audio](/developer/runtime/audio/).

**68020** — The 32-bit successor of the 68000. It adds long branch displacements, bitfield instructions and full-format addressing modes.

**68EC020** — A 68020 variant with a 24-bit address bus. It is the F3 main CPU. `Machine::read8` masks every address with `0xffffff`. The main CPU runs at 16 MHz (`Machine::main_clock`). The interpreter uses the Musashi type `M68K_CPU_TYPE_68EC020`.

**93C46** — A serial EEPROM with 64 words of 16 bits. The F3 stores settings in it. The class is `Eeprom` in `runtime/eeprom.hpp`. It has a busy time after each write.

## A

**ABI (application binary interface)** — The C interface between the generated code and the runtime. It is the file `include/f3rt/cpu_abi.h`. The runtime owns it. See [CPU ABI](/developer/runtime/cpu-abi) and [Architecture](/developer/architecture).

**ABI version** — `F3RT_ABI_VERSION` in `cpu_abi.h`, currently 3.
Both generated CPU programs reject a version mismatch during compilation.

**A-line and F-line opcodes** — 68000 opcodes whose first four bits are `1010` (A) or `1111` (F). They raise exception vectors 10 and 11. Generated code handles them natively with `f3_exception`.

**all_aligned** — A discovery mode that independently decodes each nonexcluded even ROM offset.
Decodes can overlap and can interpret data as instructions.
The mode does not prove reachability or supported lowering.
See [Discovery](/developer/recompiler/discovery).

**autovector** — An interrupt vector that the CPU computes from the IRQ level. The main CPU uses vector `24 + level`.

## B

**block** — One generated C function (`f3_native_XXXXXX`) that runs several consecutive instructions. A table entry (`f3_block`) maps a PC address to the function. Many table entries can point to the same function. See [Code emission](/developer/recompiler/emission).

**boundary** — The point between two blocks, or between two instructions inside a block when the deadline is due. The runtime function `Machine::boundary` runs there. It advances the devices and delivers interrupts.

## C

**Capstone** — A disassembler library. The Python recompiler uses version 5.0.9 to decode 68020 instructions. Some Capstone fields are wrong for some addressing modes. The code works around them. See [Code emission](/developer/recompiler/emission).

**compare mode** — The developer renderers `--renderer compare-cpu` and `compare-gpu`. It runs `GameVideo` and the FDP renderer for each frame. The program stops with an error if the pixels differ for a supported frame.

**coverage** — The report of what the recompiler found. The file is `coverage.json`. It lists decoded instructions, rejected words and unresolved transfers. It is not proof that unknown bytes are data.

**CPU fallback** — Musashi executes a main CPU instruction instead of generated C.
A missing entry, trace mode, or unsupported lowering can request it.
Strict-native mode rejects it.

## D

**decoder rejection** — Capstone cannot produce a valid instruction at a candidate offset.
A rejection differs from an unsupported lowering.
Only recognized primary-opcode cases become generated exception entries.

**differential test** — A test that runs the same instruction in the lowered C and in the Musashi reference. It compares registers, flags, memory writes and cycles. The code is in `tools/differential/`. See [Differential testing](/developer/testing/differential).

**discovery** — The recompiler stage that finds instructions in the ROM. The Python function is `discover` in `recomp/discovery.py`.

**dispatch** — The act of finding the generated block for the current PC and running it. The function is `f3_dispatch`.

**dispatch deadline** — The field `f3_cpu::dispatch_deadline`. It is the cycle count of the next scheduled event (vblank, IRQ3 or watchdog). A generated block must return at the first instruction boundary at or after this count. Zero means "check again now". See [CPU ABI](/developer/runtime/cpu-abi).

**DPRAM (dual-port RAM)** — See *shared RAM*.

**DUART** — See *MC68681*.

## E

**EEPROM** — See *93C46*.

**ES5505** — The Ensoniq OTIS sound chip. It has 32 voices that play 16-bit samples from the sample ROM. The implementation is `runtime/third_party/audio/es5505.cpp`. Also written *OTIS*.

**ES5510** — The Ensoniq ESP signal processor (DSP). It makes effects such as delay. The implementation is `es5510.cpp`. The DUART output pin OP6 halts it.

**exception** — A 68000-family event such as a trap, an illegal opcode or an interrupt. `f3_exception` pushes the stack frame and loads the vector. It also charges the cycle cost.

**exception entry** — A dispatch table entry that raises a guest exception without interpreter execution.
The main generator recognizes A-line, F-line, and known illegal primary opcodes.
It does not classify every decoder rejection as illegal.

## F

**F3** — The Taito F3 arcade hardware. The main CPU is a 68EC020. The sound board has a 68000. The MAME driver is `taito_f3.cpp`.

**F3SND2** — The binary trace format of `SoundTrace`. The file starts with the 8 bytes `F3SND2\0\0`. Each record has 32 bytes. It stores the time, the sound-sample count, the PC, the address, the value and the kind of one bus event. `tools/decode_sound.py` reads it. See [Audio](/developer/runtime/audio/).

**fallback** — See *CPU fallback* and *video fallback*.

**FDP (TC0630FDP)** — The F3 video chip. It draws four tile playfields, a text layer and sprites. `Video` (in `runtime/renderer/fdp/video.cpp`) is a MAME-derived software model; internal parity is not physical-chip verification. See [FDP renderer](/developer/runtime/video/fdp).

## G

**game-data HLE** — `GameVideo` rebuilds supported scenes from the FDP video RAM that the game already wrote.
Unsupported frames (flipped screen, sprite trails, bitmap pivot) use the FDP renderer.
See [Game-data HLE](/developer/runtime/video/game-hle).

**GameVideo** — The class that does game-data HLE. Its modes are `Diagnostic`, `Game` and `Compare` (`GameVideoMode`).

**generated code** — The C files that `uv run python -m recomp emit` and `tools/compile_sound.py` write. They come from your ROM. Never commit them.

## H

**hook** — *Retired.* A recompiler feature that called a C function before a ROM instruction. The video scene no longer uses hooks; `GameVideo` decodes FDP video RAM at VBSTART instead. Hook support was removed from `recomp/`.

**HLE (high-level emulation)** — A method that imitates what software does instead of what hardware does. Here it means game-data HLE for video. See *game-data HLE*.

## I

**IACK (interrupt acknowledge)** — The bus cycle in which the CPU reads the interrupt vector. For the sound CPU, `Audio::irq_ack` supplies a vector from the DUART.

**input word** — A 16-bit value (`LocalInputWord`) with 14 active-high bits for one local player. Bits 0 to 3 are up, down, left, right. Bits 4 to 6 are buttons 1 to 3. Bit 7 is start, bit 8 coin, bit 9 service, bit 10 test. Bits 11 to 13 are buttons 4 to 6.

**interpreter** — The class `Interpreter` that wraps the Musashi core. It runs one main CPU instruction (fallback and reference mode) and runs the sound CPU in oracle mode. See [Interpreter](/developer/runtime/interpreter).

**IRQ** — Interrupt request. The main CPU receives IRQ level 2 at vblank and level 3 10,000 cycles later. The sound CPU receives level 6 from the DUART.

## L

**Land Maker** — The game that this project runs. The target is the Japanese version 2.01J (`landmakrj`). The World version (`landmakr`) has a config file only and is untested.

**lane** — One ROM chip that holds every Nth byte of an interleaved image. The main program has four lanes at byte offsets 0, 1, 2 and 3 (`interleave = 4`). The recompiler and `RomSet::load` both put the lanes together. See [ROM loading and config](/developer/recompiler/rom-and-config).

**lazy flags** — A method to defer the CPU flag calculation. Generated code stores the operation and its operands (`cc_op`, `cc_src`, `cc_dst`, `cc_result`, `cc_width`). `f3_cc_flush` calculates N, Z, V and C only when the code needs them. The X flag is always stored at once. See [Flags and timing](/developer/recompiler/flags-and-timing).

**lowering** — The step that turns one decoded instruction into C statements. The function is `lower` in `recomp/emitter.py`. The file `lowering.json` reports the result.

**lowering gap** — A valid decoded instruction lacks an emitter implementation.
Main generated code calls `f3_fallback` for this case.
Sound generated code reports an unsupported PC instead.

## M

**Machine** — The class `f3rt::Machine`. It owns the CPU, the memory arrays, the devices and the scheduler. See [Machine](/developer/runtime/machine).

**mailbox** — The shared RAM at `0xc00000` (main CPU side) with the reset lines at `0xc80000` and `0xc80100`. The main CPU uses it to send commands to the sound CPU.

**MAME** — A multi-system emulator. The project uses a MAME build as an external reference for pictures and sound. The Lua scripts in `tools/mame/` capture its output. See [MAME oracle and captures](/developer/testing/mame).

**MB87078** — An electronic volume chip on the sound board. The class is in `third_party/audio/mb87078.cpp`. It sets the output gain.

**MC68681 (DUART)** — A Motorola dual UART. On the sound board it gives the sound CPU a timer interrupt and serial channel status. Its pin OP6 controls the ES5510 halt line. The class is `third_party/audio/mc68681.cpp`.

**Musashi** — An open-source C emulator of 68000-family CPUs. The project vendors it in `runtime/third_party/musashi`. It is the interpreter, the reference core and the source of the cycle tables. The copy has patches for MAME parity.

## N

**N64Recomp and N64ModernRuntime** — Nintendo 64 projects that separate static translation from the hardware runtime.
f3-recomp follows this separation. See [Architecture](/developer/architecture).

**native** — Code that runs as compiled C instead of through an interpreter. "Native main CPU" means the generated blocks. "Native sound" means `SoundNative`, which translates the sound ROM's CPU instructions while retaining emulated devices; it is not HLE.

## O

**oracle** — A reference implementation used for comparison.
Examples include Musashi, the FDP renderer, and MAME.
A disagreement needs investigation; it does not prove which side is wrong.

**OTIS** — Another name for the ES5505 chip. See *ES5505*.

## P

**PCM** — Pulse-code modulation: digital audio samples.
The runtime generates signed 16-bit stereo samples at the emulated ES5505 rate (29761 Hz with 32 active voices); host playback can resample them.
See [Audio timing](/developer/runtime/audio/timing).

**playfield (PF)** — One of the four tile layers of the FDP. The code names them PF0 to PF3.

**producer** — A game routine that builds display data.
Examples include tile blocks, sprite lists, text strings, and line profiles.
`GameVideo` does not observe producers; it decodes their output from FDP video RAM
at VBSTART. With `--discovery-log` the runtime also logs producer store PCs
that are not in a game's known list.

## R

**ROM set** — The group of ROM chip files for one game version. `RomSet` in `rom.hpp` holds the regions: `main`, `sprites`, `sprites_hi`, `tiles`, `tiles_hi`, `sound` and `samples`. The set names are `landmakrj` (Japan) and `landmakr` (World).

## S

**sample ROM** — The ROM region `RomSet::samples` (16 MiB). It holds the 16-bit sound samples that the ES5505 plays.

**scanout** — The 432 by 256 grid of positions that the video model uses before it crops to the visible 320 by 232 picture.

**scene** — Renderer-independent display data built by the game-data producers.
`SceneRow`, `SceneSprite`, and related records describe the pixels, positions, clips, priorities, and mixing rules.
See [Scene model](/developer/runtime/video/scene).

**shard** — One generated C file that holds many block functions. The main compiler puts up to 128 blocks in a shard. The sound compiler puts up to 1024.

**shared RAM** — The 2 KiB RAM (`Machine::shared`) visible to both CPUs. It is at `0xc00000` for the main CPU and at `0x140000` for the sound CPU. The sound CPU reads only the even byte lane.

**snapshot** — A frame-boundary machine copy. `Machine::save_state` and `load_state` write and read it for save-state slots and tests, and `state_crc` checksums it.

**sound driver** — The program that runs on the sound CPU. It reads commands from the mailbox and controls the ES5505. The project runs it natively (`SoundNative`) or in the interpreter (oracle). See [Sound-CPU compiler](/developer/recompiler/sound-compiler).

**SoundNative** — The class that runs the statically compiled sound driver. It has its own `f3_cpu` state. It never calls Musashi.

**sprite lag** — The FDP draws each frame with the sprite list of the previous frame. `Video::vblank` and `render_frame` keep this one-frame buffer.

**static recompilation** — Translating a program to another language before it runs. Here: 68020 machine code to C.

**STOP** — A guest instruction that stops instruction execution until an eligible interrupt.
It differs from the runtime's fatal `halted` state.
`Machine::boundary` advances time to the next event while the CPU is stopped.

**strict-native** — The mode of the `landmakr` executable. Generated code runs the main CPU, and any untranslated instruction is an error. Video fallback is separate and does not break this mode.

## T

**TC0630FDP** — The full name of the *FDP*.

**trace mode** — The main CPU's T0 or T1 status bits request instruction-level trace handling.
`f3_dispatch` sends trace-mode execution to Musashi.
Strict-native execution therefore rejects this path.

## V

**vblank** — The start of the vertical blanking period. In this runtime it is the moment when the machine renders the frame and raises IRQ2. The time zero of the raster is the reference screen's VBSTART epoch.

**vector table** — The table of 256 long addresses that the `VBR` register points to. Entry 0 is the initial stack. Entry 1 is the reset PC.

**video fallback** — `GameVideo` draws a frame with the FDP renderer because a supported-feature test failed (flipped screen, sprite trails, bitmap pivot). The program counts these frames and prints them at the end of a run.

**visible area** — The native output crop: 320 by 232 pixels.
It differs from the 432 by 262 raster timing geometry and the 432 by 256 scene scanout space.
See [Video hardware](/developer/runtime/video/hardware).

## W

**watchdog** — A timer that resets the machine if the game does not strobe it. The game writes to `0x4a0000`. The machine resets three seconds of main-clock cycles after the last strobe.

**worktree (`wt/`)** — A local Git worktree. The folder is ignored and is not part of the distributed source tree.

## X

**X flag** — The 68000 extend flag. Generated code stores it at once, not lazily, because partial flag updates must keep it.
