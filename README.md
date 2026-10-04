# f3-recomp

Taito F3 static recompilation + modern runtime, modeled on N64Recomp + N64ModernRuntime.
Execution target: Land Maker Japan 2.01J (`landmakrj`, main CPU 68EC020), using the supplied ROM directory named `landmakr`. World `landmakr` is configuration-only and untested: its e61-19..16 program lanes were not supplied.

## Documentation

Read the [user guide, CLI reference and Developer documentation](https://ansxor.github.io/f3-recomp/).
The VitePress source is in [`docs/site`](docs/site).
The Developer section covers architecture, code emission, hardware models, netplay and verification.

Use Node.js 22 or newer to run the documentation locally:

```sh
cd docs/site
npm ci
npm run dev
```

Use `npm run build` to generate the site. Use `npm run preview` to inspect the production output.
Documentation builds do not require ROM files.
The [Pages workflow](.github/workflows/docs.yml) builds pull requests and deploys documentation changes on `main`.
You can also start it with **Run workflow** in GitHub Actions.

## split
- `recomp/`   tool: ROM -> C (68020 lifter, function discovery, jump-table/indirect handling, per-game TOML config). Output C calls only the runtime ABI.
- `runtime/`  library `f3rt`: memory map + I/O, interrupts/vblank timing, FDP (video), OTIS/ES5505 + sound CPU (audio), input, EEPROM, SDL3 frontend.
- `include/f3rt/` the ABI between the two. Owned by runtime; recomp consumes. Change it only with a note in docs/ABI-CHANGES.md and tell the other session.
- `games/landmakr/` per-game config + generated C (generated C and ROM data are gitignored).
- `tools/mame/`  MAME lua scripts that dump traces (memory writes, regs, frames, audio) for differential testing.

ROMs live outside the repo: `../roms/<set>/`.

## Build and run the integrated game

Build this checkout. Prerequisites: CMake, Ninja,
a C/C++20 compiler, SDL3 development files, Python 3.11+, and Capstone 5.0.9.
Install the Python dependency once with
`python3 -m pip install --target build/python -r recomp/requirements.txt`.

From this repository root, the build-and-run command is:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR=../roms/landmakr &&
cmake --build build --target landmakr -j 4 &&
./build/landmakr
```

CMake validates the Japanese program lanes, generates C into ignored
`build/generated/landmakrj`, and links it with `f3rt`. The configured ROM
directory is the executable's default; `--rom-dir DIR` overrides it.
`landmakr` always selects generated main-CPU code and rejects any untranslated
instruction. `--allow-fallback` explicitly opts into diagnostic interpretation;
that mode does **not** satisfy native-game acceptance. The separate `f3rt-run`
and `f3rt-replay` targets are test aids. `landmakr` defaults to the native
recompiled sound driver; `--sound-driver oracle` selects its interpreted reference.

Controls: **5/6** coin, **1/2** start, **arrows** direction, **Z/X/C** buttons,
**F1** service, **F2** test, **Escape** quit. `--eeprom build/landmakr.nv`
persists settings. `--frames N --headless --wav build/audio.wav` supports
finite verification runs without changing the CPU execution path.

The build command is smoke-tested; final playable frame/audio equivalence is
tracked in `STATUS.md`, not implied by successful compilation.

### Opt-in 1v1 rollback netplay

Build the dependency-free Go relay and start it on a reachable UDP port:

```sh
(cd netplay/server && go build -o ../../build/netplay-server .)
build/netplay-server -addr 0.0.0.0:9000
```

Use matching game builds and ROMs, then run on each client:

```sh
build/landmakr --netplay-server SERVER:9000 --netplay-room example --netplay-player 1 --netplay-delay 2
build/landmakr --netplay-server SERVER:9000 --netplay-room example --netplay-player 2 --netplay-delay 2
```

Keys control the assigned local player: arrows, Z/X/C, either start key,
either coin key, F1 service, F2 shared test. Coins/service/test are synchronized;
focus loss releases inputs through the same delayed path. Both peers cold-boot
the erased factory EEPROM; persisted settings and diagnostic execution modes
are rejected. The title shows session state, RTT and rollback depth.

Full usage, versus entry, protocol/state inventory, exact-reference oracle,
impairment tests and measured rollback limits: [docs/NETPLAY.md](docs/NETPLAY.md).
The default history retains 16 rollback frames, not a promise that replaying
all 16 fits a 60 Hz display interval. Confirmed-only audio avoids duplicated
speculative output at the cost of additional audio latency.

### Game-data video and presentation

`--video game` (default for the strict-native `landmakr` executable)
reconstructs Land Maker's scene from its native display producers
and ROM/work-RAM descriptors. `--video fdp` selects the hardware-RAM
renderer (the oracle, and the default under `--allow-fallback`). `--video compare` renders both and rejects
any supported-frame RGB mismatch. Both game-data modes require strict-native
`landmakrj`; renderer fallback for explicitly unsupported frames is separate
from, and never enables, main-CPU interpreter fallback.

After native pixel parity, opt into scene rerasterization and extra border:

```sh
./build/landmakr --video game --video-scale 2 --video-border 48 --video-filter linear
```

Defaults preserve native presentation: scale **1**, border **0**, filter
**nearest**. Scale accepts 1–4; border accepts 0–160 native columns **per side**.
Border 48 gives 416×232 before internal scaling. Linear is optional final
SDL texture filtering; source ROM artwork is unchanged. Native captures/CRCs
remain 320×232. Unsupported enhanced frames show the exact oracle picture
centered with black side borders, rather than invented off-screen geometry.

ROM addresses, descriptor layouts, measured per-layer parity, fallback limits
and presentation evidence: [docs/VIDEO-HLE.md](docs/VIDEO-HLE.md).

### Sound-driver observation and native execution

`--sound-trace FILE` on `landmakr`, `f3rt-run`, or the seeded gameplay harness
records the selected sound driver's device reads/writes and main-CPU mailbox
writes without changing dispatch deadlines or reading registers a second time.
Keep traces and extracted events under ignored `build/`; they contain ROM-derived
data. The gameplay harness also accepts `--wav FILE`.

```sh
build/f3rt-gameplay-regression --seed 5 --frames 6000 --sound-driver oracle \
  --sound-trace build/seed5.sound --wav build/seed5.wav
python3 tools/decode_sound.py build/seed5.sound \
  --output build/seed5-writes.jsonl.gz
```

`--notes-only` extracts voice starts rather than every bus/register update.
Device timestamps use the effective 16 MHz device cursor and emitted-sample
ordinal, not the enclosing main-CPU block's endpoint. Register snapshots expose
sample-ROM word addresses, pitch increment, loop/direction, L/R volume, filter,
bank and output pair. See [docs/SOUND-DRIVER.md](docs/SOUND-DRIVER.md).

Sound defaults to the independent, statically recompiled ROM driver when the
build generated it (`F3_ROM_DIR`); it does not replay traces or call the sound
interpreter. `--sound-driver oracle` selects the interpreted reference driver. ES5505/ES5510 and SDL3 remain unchanged. Configuring with
`F3_ROM_DIR` generates both main and sound programs.

```sh
build/f3rt-gameplay-regression --seed 5 --frames 6000 --sound-driver native \
  --sound-trace build/seed5-native.sound --wav build/seed5-native.wav
python3 tools/compare_sound.py build/seed5.sound build/seed5-native.sound
cmp build/seed5.wav build/seed5-native.wav
```

The comparator requires exact records, timestamps and ownership metadata.
The seed-5 gate matches 12,258,121 records and complete WAV bytes.

Extract a music sequence after normal game initialization, then decode its
command/note/voice timeline:

```sh
build/f3rt-sound-extract --rom-dir /path/to/roms/landmakr \
  --sound-driver native --packet 038108 --packet 04860874 --seconds 5 \
  --wav-window event --sound-trace build/music.sound --wav build/music.wav
python3 tools/decode_sound.py build/music.sound --notes-only \
  --output build/music.jsonl
```

The extractor freezes the main CPU after 900 boot frames, drains its pending
mailbox packets, and schedules `--packet HEX` / `--at SECONDS:HEX` against sound
time. The default wait includes the game's output-gain writes at 13.23 seconds;
earlier `--boot-frames` values can retain startup attenuation. No hidden setup
packets are injected. Full traces retain boot; `--wav-window event` trims only
the WAV. See [docs/SOUND-DRIVER.md](docs/SOUND-DRIVER.md) for SFX, timing, ROM
evidence and non-exhaustive native-mode limits.




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

- Japan uses `[discovery].coverage = "all_aligned"`: independently decode every
  even ROM offset, including instruction starts inside another instruction's
  extension words. Computed jumps, odd-offset script pointers, and long
  straight-line routines do not depend on observed-PC lists or pointer scans.
  This deliberately includes data that resembles code; it is not a
  reachability classifier. Decoder rejections and unsupported lowerings remain
  explicit in `coverage.json` and `lowering.json`, never silently accepted.
- Every decoded instruction PC is registered, including overlapping entries.
  All-aligned blocks group at most 32 word positions in a 64-byte ROM page;
  execution follows the decoded instruction length, not the next candidate
  word. Known illegal/A-line/F-line words use native architectural exception
  handlers, not interpretation. Emulated calls use the guest stack, not
  recursive host calls. ABI v2's `dispatch_deadline` ends a block
  at the first instruction boundary reaching a scheduled event. Lowering the
  IRQ mask invalidates the deadline so pending interrupts are reconsidered.
  Generated C and headers reject incompatible runtime ABI versions.
  Diagnostic execution may interpret unknown or RAM PCs one instruction at
  a time. The integrated `landmakr` target rejects these by default; fallback
  is not native-game acceptance.
- NZVC flags are lazy inside blocks; X is retained eagerly for partial flag
  updates. Native exits materialize SR before the runtime boundary/IRQ check.
  Only ordinary bus reads/writes may observe pending flags. Trace-enabled
  execution must use runtime fallback, not multi-instruction native blocks.
- Scheduling uses pinned Musashi 68EC020 opcode costs, with observed MAME
  corrections for MOVEM stores, fixed-cost rotates, and `TRAP #n`, plus
  full-index extensions, conditional branches, and loop expiration. These are
  reference-emulator timings, not physical bus-cycle accuracy. Deadline yields
  preserve multi-instruction blocks and lazy flags without delaying scheduled
  IRQs to the original static block end. Frame/audio parity is checked
  separately against observed Land Maker output.
- Native bus callbacks synchronize device time before sound-mailbox reads,
  writes, and reset-line changes. This prevents main-block interior accesses
  from becoming visible to earlier sound execution without adding CPU cycles,
  forcing single-instruction blocks, or delivering IRQs inside an instruction.
- The optional `coverage = "recursive"` mode retains vector/config roots and
  bounded table/task/callback scans for other configurations. Its explicit
  `entry_points` and `[[discovery.jump_tables]]` (`address` plus `targets`,
  or `table` plus `count`) are not used to establish Japan coverage.
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

### Strict seeded gameplay regression

```sh
cmake --build build --target f3rt-gameplay-regression -j 4
python3 tools/run_gameplay_regression.py --rom-dir ../roms/landmakr \
  --frames 40000 --seeds 1 2 3 4 5 6 7 8
```

The headless executable runs the real machine, renderer and sound hardware.
Each seed starts cold, inserts a coin at frames 700/720, pulses start every
90 frames during 800–2399, then applies the crash reproducer's 64-bit LCG
direction/Z/X/C schedule every six frames from frame 1200. It rejects fallback,
CPU halt, and execution errors, reporting the seed, frame and CPU state.
`--binary` selects an isolated build; the executable's `--seed`, `--frames`,
`--dump-dir` and `--surface` options reproduce individual runs and captures.
Passing seeds are sampled gameplay evidence, not proof that every possible
game state has executed.

Add `--video-diff` to compare the game renderer's four playfields, four sprite
priority planes, text, visible row descriptions and final native RGB against
the FDP oracle. Sampling starts at frame 600, every 120 frames by default;
`--video-diff-every N` changes that interval. `--video-layer-mask N` selects
bits 0–3 (PF), 4–7 (sprites) and 8 (text); default 511 also compares composition.
Each layer reports exact compared-pixel and mismatch counts. Unsupported
sampled state fails explicitly; it is not counted as a matching HLE frame.
The executable's `--dump-dir DIR` also captures the first video mismatch.


## Instruction-level differential self-test

```sh
PYTHONPATH=build/python python3 -m unittest discover -s tools -p 'test_*.py'
PYTHONPATH=build/python python3 tools/differential/run.py \
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
EC020 MOVEM stores cost three cycles/register, rotates have no count surcharge,
and `TRAP #n` takes 24 cycles. See [ABI changes](docs/ABI-CHANGES.md).
Synthetic generated-code tests exercise deadline equality/overshoot, resumption
at an interior PC, and materialized flags on each yield.

## MAME capture

`tools/mame/capture.lua` captures aligned frame/state bundles, CPU state,
main RAM, video/palette/control RAM, active sprites, and audio-state evidence.
See [the capture protocol and commands](tools/mame/README.md).
Use format-2 captures: MAME's frame-done pixel API returns the preceding
completed bitmap, so the script defers pixel capture one callback to align it
with the saved machine state. Old format-1 bundles are not valid parity pairs.
Keep all captures under ignored `build/`.
