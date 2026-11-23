---
name: cpp-to-rust
description: "Port a C++ subsystem in f3-recomp into a Rust workspace crate bridged via cxx.rs and Corrosion: plan incremental waves, do a bit-exact mechanical 1:1 port first with direct Rust bindings (never C++ vtable shims), verify parity and headless isolation, then shrink the bridge, confine unsafe/raw pointers to thin RAII FFI boundaries, and add native proptest. Use whenever porting a C++ runtime subsystem to Rust."
---

# cpp-to-rust: porting an f3-recomp C++ subsystem to Rust

Never mix behavioral redesign with translation. Every port runs in two strictly separated phases:

1. **Mechanical 1:1 port** — preserve exact algorithms, packed layouts, save-state bytes, and oracle/CRC parity across small waves, ending in a verified checkpoint.
2. **Cleanup & safe-boundary pass** — once parity is locked in, retire dead C++ façades/tests, shrink `crates/f3rt-abi`, apply `pseudocode-architecture` receiver/type design, confine `unsafe` and raw pointers to thin RAII FFI seams, and add native Rust `proptest`.

## Preferred model routing (T3 Code via OMP)

Unless the user specifies different models for a run, route delegated waves (`delegate_task`, `isolation: "shared"`, `providerInstanceId: "acpRegistry_omp"`) as follows — always check `orchestrator_capabilities` first to confirm availability:

- **Phase 1 — Mechanical 1:1 port (Waves 0–4):** `opencode-go/deepseek-v4.1-flash` (fast, faithful translation and build/FFI wiring).
- **Phase 2 — Cleanup, bridge shrinkage, safe FFI boundaries & `proptest` (Waves 5–6):** `openai-codex/gpt-6.2-sol` with `options: {"thinking": "high"}`.
- **Deep `proptest-praxis` / verification fallback** (e.g. if `gpt-6.2-sol` hits provider limits): `opencode-go/glm-5.3` with `options: {"thinking": "max"}`.

## Non-negotiable project rules

- **One staticlib (`crates/f3rt-abi`).** Every new subsystem lives in its own `rlib` crate (`crates/f3rt-<name>`, `edition = "2024"`) depended on by `crates/f3rt-abi`. Never add a second Rust `staticlib` (it duplicates `std`).
- **Owned `rust::Box<T>` over `cxx.rs`, not singletons.** Wire each bridge via `corrosion_add_cxxbridge(f3rt_<name>_cxx CRATE f3rt_abi FILES <name>.rs)` in `cmake/corrosion.cmake`. Do not imitate the process-wide singleton C ABI in `crates/f3rt-audio` / `crates/f3rt-eeprom`, and leave those crates untouched unless explicitly porting them.
- **Bind native libraries directly in Rust — never write a C++ driver/vtable shim.**
  - Use direct Rust bindings (`sdl3-sys`, `objc2`, etc.) gated behind an optional Cargo feature. Never introduce a C++ function-pointer table shim or keep Objective-C++ (`.mm`) helpers that `objc2` can own cleanly in Rust.
  - When CMake already owns a native library (e.g. `SDL3::SDL3`), use `no-link` in Cargo and link it on `f3rt_abi INTERFACE` inside `cmake/corrosion.cmake`.
  - **Corrosion does not propagate Rust `native-static-libs`.** For Apple frameworks used by `objc2-*` (e.g. `AppKit`, `QuartzCore`, `CoreFoundation`), add explicit `-framework <Name>` to `f3rt_abi INTERFACE` under `if(APPLE)` inside the matching CMake option guard.
- **Keep headless builds 100% leak-free.** Every GUI/SDL/GPU/platform-framework dependency must be `optional = true` (and `default-features = false` with minimal features) gated behind a Cargo feature enabled in `cmake/corrosion.cmake` only when the corresponding CMake option (`F3RT_SDL`, `F3RT_GPU`, etc.) is `ON`.
- **Keep `proptest` in `[dev-dependencies]` only** so production and CMake builds never compile or link it.
- **Pin binary & hardware contracts.**
  - Assert every packed save-state record size and `#[repr(C)]` struct size/offset with `const _: () = assert!(...)`.
  - Never modify independent differential oracles (e.g. `runtime/renderer/fdp/video.cpp`) to match the port.
  - Follow `AGENTS.md` (12's board-verified hardware observations outrank MAME) and repo guards (e.g. `cmake/check_game_geometry_literals.cmake`).
  - Keep `build_identity` accurate: `CMakeLists.txt` hashes `crates/f3rt-*/src/*.rs` recursively; verify new simulation files are covered.

## Wave playbook

### Wave 0 — Plumbing & leaf cutover
1. Create `crates/f3rt-<name>` (`Cargo.toml`, `src/lib.rs`) and add it to workspace `Cargo.toml` and `crates/f3rt-abi/Cargo.toml`.
2. Add `crates/f3rt-abi/src/<name>.rs` (`#[cxx::bridge(namespace = "f3rt::rust_<name>")]`), wire `corrosion_add_cxxbridge` in `cmake/corrosion.cmake`, and register `f3rt-<name>-cargo-test` in `CMakeLists.txt`.
3. Port pure leaf modules (config, constants, validators, small decoders) and cut over **one** small leaf live across `cxx.rs` to prove CMake + Corrosion + `cxxbridge` end-to-end before porting the bulk.

### Waves 1–2 — Bottom-up mechanical port (Rust-only, no C++ churn)
Port data layouts, state serialization, decoders, and core engines bottom-up in cohesive waves.
- Mirror C++ algorithms and edge cases 1:1; do not "improve" behavior yet.
- Add unit tests in each module pinning sizes, byte layouts, edge cases, and exact save-state byte count.
- Verify `cargo test -p f3rt-<name>`, `clippy --all-targets -- -D warnings`, and `cargo fmt --check` after every wave.

### Wave 3 — Live cutover & differential verification
- Switch the C++ subsystem entry points to hold `rust::Box<T>` and forward to Rust.
- Delete the replaced C++ implementation files immediately (no dead parallel copies).
- Run full unit tests, CTest, and live ROM differential/CRC checks. Fix every parity discrepancy against the C++ oracle/CRC before moving on.

### Wave 4 — External/platform backend (if applicable)
- Port any SDL3, GPU, OS, or Objective-C++ backend onto direct Rust bindings (`sdl3-sys`, `objc2` with `default-features = false`, etc.) behind a Cargo feature flag, with a lightweight stub module when the feature is off so the `cxx.rs` bridge stays unconditional and headless builds stay library-free.
- Delete any `.mm` files and `enable_language(OBJCXX)` once Objective-C++ is gone.
- Verify feature-on **and** feature-off (headless) builds, leak audit, and live execution.
- **Stop here for a clean commit checkpoint** before starting cleanup.

### Wave 5 — Bridge shrinkage, idiomatic cleanup & native `proptest`
1. **Audit C++ callers:** grep `runtime/`, `tools/`, `include/`, `games/`, and `CMakeLists.txt` for every remaining C++ wrapper header and library target.
2. **Migrate pure unit & RapidCheck tests to Rust:** move subsystem unit tests and property tests out of `runtime/tests/` into `crates/f3rt-<name>` using `proptest` (`[dev-dependencies]`). Keep C++ tests that genuinely test C++↔Rust integration or the C++ oracle.
3. **Delete dead C++ façades & shrink `crates/f3rt-abi/src/<name>.rs`:** remove every C++ wrapper file, CMake static library, and `#[cxx::bridge]` function/POD that only existed for migrated tests.
4. **Idiomatic cleanup:** replace byte-cast callbacks with typed slices, reuse scratch buffers, remove redundant clones, and strip mechanical port/narration comments (keep only `// SAFETY:` invariants and non-obvious hardware reasons).

### Wave 6 — Safe FFI boundary & pseudocode-architecture pass
Apply `pseudocode-architecture` and `zachary-pseudocode-architecture` together with strict `unsafe` minimization:
- **Confine all `unsafe` and raw pointers to thin FFI modules.** Domain/logic modules should have **0 `unsafe`** and **0 `*mut`/`*const` pointers** (replace `slice::from_raw_parts` uniform transmutes with explicit `to_le_bytes()`, replace C timer FFI with `std::time::Instant`, replace pointer-to-`usize` casts with typed handle refs).
- **Scoped RAII receivers at FFI boundaries.** Wrap raw handles privately (`NonNull<T>`) and return scoped pass/guard objects whose `Drop` closes the resource (e.g. `CommandBuffer::begin_render_pass(&mut self) -> RenderPass<'_>`) instead of passing raw pointers back into a parent handle.
- **Eliminate `#[allow(...)]` thoughtfully.** Remove crate-level `#![allow(clippy::not_unsafe_ptr_arg_deref)]` via typed wrappers, and replace genuine multi-parameter clusters (`#[allow(clippy::too_many_arguments)]`) with cohesive domain structs. Never invent synthetic `FooArgs` bags or trait/vtable layers just to satisfy a lint; preserve `#[cxx::bridge]` signature allows only where C++ ABI compatibility requires them.

## Verification checklist (run after Waves 3, 4, 5, 6)

1. **Rust checks (default + every optional feature):**
   ```sh
   cargo test -p f3rt-<name>
   cargo clippy -p f3rt-<name> -p f3rt-abi --all-targets -- -D warnings
   cargo clippy -p f3rt-<name> -p f3rt-abi --features <feat> --all-targets -- -D warnings
   cargo fmt -p f3rt-<name> -p f3rt-abi -- --check
   ```
   On macOS with Nix `cc`/`c++` wrappers on `PATH`, linking `f3rt-abi` tests or `-liconv` may require Apple Clang explicitly:
   ```sh
   CC=/usr/bin/clang CXX=/usr/bin/clang++ RUSTFLAGS="-C linker=/usr/bin/clang++" cargo test --workspace
   ```
2. **CMake full & headless builds (macOS Clang):**
   Configure and build both `build/<game>` (full) and `build/<game>-headless` (`-DF3RT_SDL=OFF -DF3RT_GPU=OFF`) with `-DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++`.
3. **CTest (sequentially):**
   ```sh
   ctest --test-dir build/<game> --output-on-failure
   ctest --test-dir build/<game>-headless --output-on-failure
   ```
   Do **not** run both CTest suites concurrently — `netplay-relay.LoopbackFixture.*` shares a temp directory and can flake under parallel CTest runs.
4. **Headless leak audit:**
   - `cargo tree -p f3rt-abi` (default features) contains zero optional GUI/SDL/platform crates and zero `proptest`.
   - `otool -L` and `nm -u` on headless executables show zero forbidden frameworks/libraries/symbols.
5. **Runtime parity & safety metrics:**
   - Verify exact save-state size & deterministic round-trip, `state_crc` stability, and any subsystem-specific live checks (e.g. `--compare-gpu --frames 300` with `pixel_mismatches=0`, `--fast-boot off` live window/pacing smoke).
   - After Wave 6, report before/after counts of `unsafe` blocks per module, raw pointers outside FFI modules, `#[allow]` annotations, and `crates/f3rt-abi/src/<name>.rs` line count.
