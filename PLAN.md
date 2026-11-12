# C++ → Rust conversion plan

## Objective

Convert the C++ runtime to Rust incrementally, from the inside out. Each subsystem boundary becomes a
C ABI; the Rust crate behind it replaces the C++ implementation and is linked through Corrosion.

**Status: `Audio` and `Eeprom` are Rust, linked, and the C++ versions are deleted.** `SpriteUnits`,
`GameVideo` and `Machine` are still the original C++ and are the next candidates (in that order,
inside out). The `GameVideo` ↔ `Machine` `friend` coupling is to be removed with the user-approved
Option A: `Machine` exposes the buffers `GameVideo` reads and writes (`m_pixels`, RAM views) through
an explicit init-time interface instead of `friend class GameVideo`.

## Process per subsystem

1. **Audit** the C++ interface and its consumers (who calls what, threading, ownership, state size).
2. **C ABI seam**: a hand-written header under `include/f3rt/abi/`, a process-wide singleton, and a
   thin C++ shim (for `Machine`: a lease that owns init/shutdown). No behaviour change. Gate.
3. **Rust crate** under `crates/`: a mechanical 1:1 translation of the C++ (same algorithms, same
   state layout, same order of operations). Unit tests port the C++ ones. Gate.
4. **Link and delete**: add the crate to `crates/f3rt-abi`, delete the C++ implementation, its
   tests that exercised it, and its build rules. Gate.

A step is either "interface → seam" or "implementation → Rust behind a seam", never both.

## Constraints

- One staticlib: `crates/f3rt-abi` is the only crate Corrosion links; the runtime crates are rlib
  dependencies of it (each staticlib embeds its own `std`; two would duplicate symbols).
  `cmake/corrosion.cmake` imports it; the flake provides cargo, rustc and the Corrosion source.
- C declarations live in `include/f3rt/abi/*.h`. Plain data only across the boundary: opaque
  handles, `repr(C)` structs, pointer + length, status codes. Whoever allocates frees.
- Mechanical and 1:1. Rewrites and optimisations come later, except measured regressions (below).
- FAUST DSPs are generated as Rust by `faust -lang rust -double`; sources and `regen.sh` live in
  `crates/f3rt-audio/faust/`, generated code is checked in under
  `crates/f3rt-audio/src/hle/faust/generated/`. FAUST is not needed to build.
- Error policy: an internal failure (synth worker error, exhausted command IDs, use before init)
  prints and aborts; no panic or exception crosses the ABI. Status codes only where a caller can
  act on them: init (`UNSUPPORTED_ROM`, `ALREADY_INITIALIZED`), `load_state` (`INVALID_STATE`) and
  EEPROM file IO (`INVALID_FILE`, `IO_ERROR`).
- Release profile (`Cargo.toml`): `opt-level = 3`, `lto = "fat"`, `codegen-units = 1`,
  `panic = "abort"`. `SampleWords::interpolate*` use `to_int_unchecked` (the C++ `size_t(position)`);
  the saturating `as usize` cost the audio worker 20%.
- Allowed to break while converting: the Nintendo 3DS build (`runtime/frontend/n3ds/**`,
  `cmake/n3ds.cmake`; no Rust cross-compile and it still names the removed audio C++ API), host tools
  (`tools/**`, `f3rt-tool`, `f3rt-runner`, `f3rt-replay`) where they used the removed audio engine
  (clock/observer APIs), and netplay over a live relay. They must break only where the audio/eeprom
  cutover or the interpreter/fast-boot removal forces it.
- Also removed on this branch, independent of the Rust work (drop it from the commit if unwanted):
  fast boot (`runtime/frontend/fast_boot.*`, `game_boot.hpp`, `games/*/boot/`, flags, settings, UI)
  and the Musashi `Interpreter` (`runtime/interpreter.*`, `core_state.c`, the Musashi build rules,
  `f3rt-run`, `Machine::run_frame(bool)`, `fallback_instructions`, `allow_main_fallback`,
  `SpriteUnits::interpreted_instruction`). A dispatch miss is now fatal. `f3rt-tool sound-extract`
  (`tools/commands/sound_extract.cpp`, `tools/runner/sound/capture.*`) is gone with it: it booted and
  drove the sound requests through the interpreter and read the audio clock (`games/*/sound/profile.cpp`
  and `tools/runner/sound/profile.hpp` stay, unbuilt). The interpreter-only cases of `runtime/tests/cpu.cpp`
  (`Movem`, `RotateCycles`, the interpreted halves of the TRAP/RTE/fallback tests) are removed.
- The audio board and EEPROM are process-wide: only one `Machine` may exist at a time. Tests that
  compared two live machines (`Cpu.WideBusBoundaries`, the `SpriteUnits`/`DiscoveryLog` comparisons,
  `GameVideo` oracle/save-load) now run their sides one after the other.
- The gate (landmakrj, ROMs in `roms/landmakrj`): 900 frames headless without audio must print
  `FINAL state_crc=0xb0ca5762` and write a WAV with sha256
  `e974016fa867b5fe58125220a4240a6f2024cc1be5248bf1b902de7ca0e3a6cd`. Every step is also compared
  against the last all-C++ binary (1800/5400 frames, `--eeprom` save+reload, `--dump-dir`, stdout
  summary lines; audio-enabled WAVs are not bit-reproducible run to run, so compare the common
  prefix) and for throughput (5400 unthrottled frames, interleaved runs).

## Done

- [x] Audio: `include/f3rt/abi/audio.h`, `crates/f3rt-audio` (mechanical translation of
      `runtime/audio/` + `hle/`, FAUST as Rust), `Machine::AudioLease`. The C++ engine, HLE, FAUST C++
      path, replay tool and the tests that drove them are deleted, and so is the MAME-derived reference backend
      (`runtime/audio/reference/`, `runtime/third_party/audio/`): HLE is the only audio implementation, so
      there is no reference to compare against. `f3rt-replay` keeps only its video capture replay.
- [x] Eeprom: `include/f3rt/abi/eeprom.h`, `crates/f3rt-eeprom` (13 unit tests), `Machine::EepromLease`.
      `runtime/eeprom.hpp` and its C++ test are deleted.
- [x] Link: `crates/f3rt-abi`, `cmake/corrosion.cmake`, workspace `Cargo.toml`/`Cargo.lock`, flake.
- [x] fast boot and `Interpreter` removed (above).

Measured on landmakrj (all-C++ binary from before the Rust work as baseline): gate crc/WAV as above;
1800 frames `0xfa5ea3c8`, 5400 frames `0xbb9eed3b`, `--eeprom` save+reload image, `--dump-dir` and
stdout identical; audio WAV of 5400 frames identical to a baseline run (the baseline itself differs
from run to run at ~2.26M frames); unthrottled 5400 frames 0.470 s (Rust audio) vs 0.441 s (C++),
min of 10 interleaved runs (+6.5%, +50 ms user CPU; not profiled: the Rust synth worker, whose drain
ends the run, or the C ABI calls from the C++ `Machine`).

## Next

1. `SpriteUnits` (`runtime/sprites/`): audit, ABI (`abi/sprite_units.h`), crate, link.
2. `GameVideo` (`runtime/renderer/game/`): remove the `friend` coupling with `Machine` (Option A),
   then ABI, crate, link.
3. `Machine` + `cpu_abi.cpp`: ABI, crate, link; the C++ shell (frontend, GPU renderer, netplay,
   FDP video, `RomSet`) stays C++ behind it.

## Reference: full Rust rewrite

Commit `8195501` on branch `rust-rewrite` (parent `5a06e6e`) converts all five subsystems in one go.
It is a reference for the remaining steps only; it was **not merged** and this branch does not
descend from it. It contains:

- `crates/f3rt-{sprite-units,game-video,machine}` plus the ABIs `include/f3rt/abi/{sprite_units,game_video,machine}.h`,
  and `runtime/machine_session.*` (`MachineSession`: `RomSet`, FDP `Video`, `DiscoveryLog` and the
  backend/discovery function tables stay C++ behind it); `runtime/machine.hpp` moved out of `include/`.
- `Machine`/`cpu_abi.cpp` translated to Rust (44 unit tests, differential run against the C++ Machine:
  per-frame `state_crc` and 43 full `save_state` dumps byte-identical on 900 landmakrj frames).
  `GameVideo` (16 tests; 60 recorded + 1,200 fuzzed frames bit-identical) and `SpriteUnits` (7 tests).
- Verified against the all-C++ binary: gate crc/WAV, 1800/5400 frames, `--eeprom`, `--dump-dir`,
  `--discovery-log[/-all]`, `--inputs`, `--renderer enhanced|accurate`, `--surface` byte-identical;
  unthrottled 5400 frames 0.479 s (C++) vs 0.483 s (Rust).
- C++ unit tests, tools and the 3DS build were left broken there by design.
