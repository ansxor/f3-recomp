# f3-recomp

Taito F3 static recompilation + modern runtime, modeled on N64Recomp + N64ModernRuntime.
Execution target: Land Maker Japan 2.01J (`landmakrj`, main CPU 68EC020), using the supplied ROM directory named `landmakr`. World `landmakr` is configuration-only and untested: its e61-19..16 program lanes were not supplied.

## split
- `recomp/`   tool: ROM -> C (68020 lifter, function discovery, jump-table/indirect handling, per-game TOML config). Output C calls only the runtime ABI.
- `runtime/`  library `f3rt`: memory map + I/O, interrupts/vblank timing, FDP (video), OTIS/ES5505 + sound CPU (audio), input, EEPROM, SDL3 frontend.
- `include/f3rt/` the ABI between the two. Owned by runtime; recomp consumes. Change it only with a note in docs/ABI-CHANGES.md and tell the other session.
- `games/landmakr/` per-game config + generated C (generated C and ROM data are gitignored).
- `tools/mame/`  MAME lua scripts that dump traces (memory writes, regs, frames, audio) for differential testing.

ROMs live outside the repo: `../roms/<set>/`.

## Recompile the supplied game

Python 3.11+ and Capstone 5.0.9 are required. Install `recomp/requirements.txt`
into your Python environment. From the repository root:

```sh
python3 -m recomp emit \
  --config games/landmakrj/config.toml \
  --rom-dir /path/to/roms/landmakr \
  --output games/landmakrj/generated
cmake -S recomp -B build/native -G Ninja \
  -DF3_GENERATED_DIR="$PWD/games/landmakrj/generated" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/native
```

`discover` instead of `emit` writes only `coverage.json`. Both commands verify
each program lane's size, CRC32 and SHA-1 against MAME's `taito_f3.cpp`, then
interleave bytes at offsets 0,1,2,3 into a 2 MiB image. No silent Japan/World
substitution.

The generated directory contains `program.bin`, sharded C, `program.h`,
`sources.cmake`, `coverage.json`, and `lowering.json`. It must remain ignored.
`libf3_recompiled.a` links with the runtime ABI; call `f3_generated_register`
once after creating the CPU, then let `f3_dispatch` run it. Alternatively include
`sources.cmake` and compile `F3_GENERATED_SOURCES`, with the repository root,
`include/`, and the generated directory on the include path.

### Discovery and execution contracts

- Vector/config entry points, direct branches/calls, bounded jump-table
  heuristics, task-entry and callback scans discover code. Reports distinguish
  explicit/vector seeds from heuristic seeds. Unreached bytes are **not**
  assumed to be data. The emitted `coverage.json` reports current instruction,
  byte, function-candidate, seed, and unresolved-transfer counts. These are
  discovery counts, not a proof of complete executable-code coverage.
- Every decoded instruction PC is registered, including block interiors.
  Native blocks have at most 32 instructions; emulated calls use the guest
  stack, not recursive host calls. Diagnostic execution may interpret unknown
  or RAM PCs one instruction at a time. The integrated `landmakr` target
  rejects these by default; fallback is not native-game acceptance.
- NZVC flags are lazy inside blocks; X is retained eagerly for partial flag
  updates. Native exits materialize SR before the runtime boundary/IRQ check.
  Only ordinary bus reads/writes may observe pending flags. Trace-enabled
  execution must use runtime fallback, not multi-instruction native blocks.
- Scheduling uses pinned Musashi 68EC020 opcode costs, including MOVEM counts,
  full-index extensions, conditional branches, and loop expiration. These are
  reference-emulator timings, not physical bus-cycle accuracy. Runtime IRQ
  delivery remains at native block boundaries; frame/audio parity is checked
  separately against observed Land Maker output.
- TOML `[discovery].entry_points` accepts observed runtime PCs.
  `[[discovery.jump_tables]]` records a transfer `address` and its `targets`,
  or a ROM `table` address and `count` of big-endian longword destinations.
  `[discovery.actor_scripts]` describes bytecode operand lengths, native callback
  commands, and script jump/call/return commands. Script roots include immediate
  stores and indexed PC-relative pointer arrays, including register-staged loads
  and configured record strides. Explicit `pointer_tables` records (`table`,
  `count`) cover script arrays passed through registers. This avoids decoding
  script words as 68020 code.
  `inline_string_helpers` identifies routines that consume an aligned
  NUL-terminated inline string after a call.
- Optional `[[hooks]]` entries have numeric `address` and C `symbol`; generated
  code calls `void symbol(f3_cpu *)` before the instruction with canonical SR.
  Changing PC or stopping the CPU skips that instruction. Link your hook
  implementation explicitly.

## Instruction-level differential self-test

```sh
python3 tools/differential/run.py \
  --musashi runtime/third_party/musashi \
  --output build/differential --cases 5000
```

This compiles literal native instruction cases and steps an independent Musashi
68EC020 core. It compares all D/A registers, PC, canonical SR, ordered bus writes,
and memory contents, with deterministic randomized states and targeted
arithmetic, addressing, privilege, exception-frame, and lazy-flag transitions.
Unsupported instructions are counted separately, never as passes. Native test
code is compiled with warnings as errors and undefined-behavior sanitization.
The reference library is built separately; its reset-cycle debt is drained and
single-instruction stepping is asserted. `--seed`, `--filter`, and
`--instructions` support reproducing or extending cases.

The runtime owns the single vendored Musashi copy. Keep the documented MAME
parity fixes in that copy: current MAME clears C on nonzero-divisor word DIV
overflow, unlike unpatched upstream Musashi. Long DIV overflow preserves C/N/Z.

## MAME capture

`tools/mame/capture.lua` captures PNG frames, CPU register TSV, and raw main RAM,
palette and video RAM. Set `F3_CAPTURE_PREFIX` to an existing output directory
plus basename, optionally set `F3_CAPTURE_FRAMES=1,60,300,600,1200`, and run a
clean MAME build with `-autoboot_delay 0 -autoboot_script tools/mame/capture.lua`.
Capture indices count frames after the script starts; the TSV also records
MAME's screen frame number. Keep all captures under ignored `build/`.
