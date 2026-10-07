# Generation and verification workflows

Run these commands from the repository root. Normal play does not require
verification tooling. Keep ROMs, generated C, captures and extracted media out
of commits; use ignored `build/` for outputs. The supported native game path
must execute with zero main-CPU interpreter fallback.

## Native game build

The [README](../../README.md#build) is the canonical integrated build recipe.
`F3_ROM_DIR` generates Japanese main and sound CPU programs during configuration.
The current [CPU ABI](ABI-CHANGES.md) is version 3; regenerate both programs when
changing it. Full-coverage tiers retain cold dispatch entries; do not use the
experimental slim build as a general-play baseline.

For standalone main-program discovery or emission:

```sh
PYTHONPATH=build/python python3 -m recomp discover \
  --config games/landmakrj/config.toml \
  --rom-dir /path/to/roms/landmakr --output build/discovery
PYTHONPATH=build/python python3 -m recomp emit \
  --config games/landmakrj/config.toml \
  --rom-dir /path/to/roms/landmakr --output build/generated-main
```

The commands validate the program lanes, not the ROM directory's name. Emitted
`program.bin`, C shards, headers, coverage/lowering reports and `sources.cmake`
are ROM-derived outputs. See [generated files](../site/reference/generated-files.md)
and [per-game config](../site/reference/game-config.md) for their contracts.

## Finite native smoke and seeded gameplay

```sh
./build/landmakr --frames 3600 --headless --wav build/attract.wav
cmake --build build --target f3rt-gameplay-regression -j 4
python3 tools/run_gameplay_regression.py --rom-dir /path/to/roms/landmakr \
  --frames 40000 --seeds 1 2 3 4 5 6 7 8
```

The seeded harness cold-boots the real runtime, inserts coins, pulses start and
applies deterministic direction/button input. It rejects CPU fallback, halt and
execution errors. A successful seed is sampled behavior, not exhaustive game
coverage. `--video-diff` compares game-data layers and native RGB against the
MAME-derived FDP reference; unsupported sampled state is not counted as a match.
See [gameplay regression](../site/developer/testing/gameplay-regression.md).

## CPU and device checks

```sh
PYTHONPATH=build/python python3 -m unittest discover -s tools -p 'test_*.py'
PYTHONPATH=build/python python3 tools/differential/run.py \
  --musashi runtime/third_party/musashi \
  --output build/differential --cases 5000
cmake --build build --target f3rt-check -j 4
./build/f3rt-check
```

Instruction differential checks compare registers, PC/SR, elapsed reference-model
cycles, ordered bus writes and memory against independent Musashi stepping.
Unsupported lowerings must be reported separately, not counted as passing.
These checks do not establish physical-bus timing or integrated video/audio
accuracy. See [testing](../site/developer/testing/index.md).

## Sound, snapshots and reference captures

- [Sound-driver investigation](../SOUND-DRIVER.md): select `--sound-driver oracle` explicitly for interpreted captures; native ROM execution is not HLE.
- [Netplay design and oracle](../NETPLAY.md): save/load, deterministic replay and impaired-relay scenarios are separate checks.
- [MAME capture tooling](../../tools/mame/README.md): format-2 state/pixel alignment; format-1 bundles are not valid comparison pairs.
- [Measurement archive](README.md): original metrics and limits, rather than a new verification claim.

Trace replay, native-versus-oracle equivalence and captured MAME-output comparison
answer different questions. None proves physical TC0630FDP or audio-board
behavior. Preserve the comparison revision, ROM identity, input schedule and
host alongside any new measurements.

## Exact runtime optimization measurements

This optimization retains full native coverage, CPU/device event ordering,
integer video blending, and the existing snapshot formats. No new GPU or HLE
approximation is introduced: GPU shaders and HLE synthesis are unchanged. The
existing accuracy limits of those backends still apply.

### Retained changes and reversal scope

| Area | Implementation references | Invariants and tradeoff | Selective reversal |
| --- | --- | --- | --- |
| Native main dispatch | `include/f3rt/machine.hpp::NativePage`; `runtime/cpu_abi.cpp::f3_register_blocks`, `f3_dispatch` | Fixed 512-entry, 4 KiB-page index; dense runs use checked arithmetic lookup, sparse runs retain bounded binary search. Cached first addresses avoid dependent table loads. The index is 8 KiB on x86-64, derived from immutable validated registration, and never serialized. Failed registration, trace gating, raw-PC eligibility, exclusions and fallback retain their behavior. | Restore global sorted-table lookup in `f3_dispatch`; remove the derived page descriptors and their registration construction together. |
| Wide main-bus access | `runtime/machine.cpp::direct_bytes`, `read16`, `read32`, `write16`, `write32` | Decode only contiguous, side-effect-free backing storage once; preserve big-endian byte assembly. Mirror/region/address wrapping, shared writes, MMIO and observed graphics writes retain ordered byte helpers. No unaligned host casts or new steady-state allocations. | Restore wide access through the existing ordered byte helpers and remove `direct_bytes`. |
| CPU composition | `runtime/renderer/game/compositor.cpp::compose_rows`, `compose_expanded_rows`, `rgb`; `runtime/renderer/game/tiles.hpp::playfield_pixel`; `runtime/renderer/game/text.hpp::pixel` | One source loop, native and scale-1..8 specializations; constant divisors, row-invariant Y sampling, bounded mosaic wrap and expanded-only last-texel reuse. Palette offsets apply to copies. Opaque 8/0 and background 0/8 weights return the exact palette RGB with alpha forced to 255; other weights retain the original saturating arithmetic. More generated code replaces repeated work; native pixel scratch shrinks to 320 entries. | Restore the generic row kernel and full RGB blend arithmetic; move the two sampler definitions back to their `.cpp` files if reversing inlining. Revert serial and worker dispatch together. |
| Accurate audio scheduler | `runtime/audio.cpp::Audio::Impl::advance` | Bound by the CPU deadline before testing the sample deadline; avoid unnecessary variable-rate division. Every slice and DUART → sample → sound-CPU edge order remains unchanged. No sound CPU or device batching. | Restore sample-deadline-first selection, keeping the same three-way minimum. |
| ES5510 execution | `runtime/third_party/audio/es5510.cpp::run_once`, `execute_run`; `es5510.hpp` | Preserve the initial HALT-released cycle, then run the same bounded 200-cycle budget in one call. Reuse the uint8 PC modulo-160 result, eliminate unused ALU metadata, and avoid division for zero/one positive delay wrap. Mutable instructions, pipeline writes, signed remainder and END behavior remain canonical. | Restore the per-cycle outer loop, original modulo expressions and operand metadata together; do not change instruction/operand/pipeline ordering. |
| ROM-access diagnostic | `tools/profile_rom_access.py::build_profile`, `instrument_generated` | Instrument successful wide-read spans as well as byte fallbacks. Include split exception translation units and their fetch hooks. Original runtime/generated sources remain untouched. | If reversing wide reads, remove their private-copy hooks with that cutover; retain split exception-unit support. |

The snapshot checker also links `f3rt_musashi` before `f3rt`: it directly uses the
Musashi producer, whose memory callbacks live in the runtime archive.

These are file/symbol reversal references, not compatibility switches. Preserve
unrelated work when reverting; restore each complete implementation rather than
leaving duplicate dispatchers, samplers or profiling paths.

### Measurement method

Baseline archives and executables were frozen from the starting worktree before
these optimizations. Both builds use GCC 16.2.1 Release, runtime `-O3 -g -DNDEBUG`,
full-coverage generated hot `-O2`/cold `-Os` tiers, the same `landmakrj` ROMs and
seed-5 single-player input schedule. Host: Ryzen 7 5700X3D, Linux x86-64; Vulkan
checks used the AMD Navi 32 adapter.

The frame probe clocks `Machine::run_frame` plus PCM draining, with no window,
wall-clock throttling, pixel/state hashing or disk writes in the timed region.
It runs 3600 frames and reports gameplay samples from frames 1200..3599. ROM
loading/decoding, registration and final state hashing are outside that region.
Each comparison alternates executable order, pins CPU 2, and uses the median of
five native pairs or three HLE pairs. Reported p95 is the median of each run's
frame-latency p95, not a pooled percentile. Keep outliers in the raw logs.

CPU composition measurements use the real Vulkan regression harness at seed 5,
1560 frames, scale 4/border 48, sampling every 120 frames with `--bench`: 100
same-scene serial/threaded presentations. Its GPU number includes a fence and
readback; it is not an unfenced interactive presentation measurement.

Hardware `perf` identified tile/text sampling and composition as the largest
combined hotspot, followed by ES5510 execution, audio scheduling and main
lookup. Callgrind comparisons isolate frames 1200..1319 with client-controlled
instrumentation, cache simulation and branch simulation enabled. Its simulated
last-level cache was rounded to 128 MiB/2-way because the physical 96 MiB cache
is not a supported power-of-two geometry; do not interpret simulated last-level
misses as hardware-counter measurements.

### Observed results

| Timed path | Baseline mean frame (ms) | Optimized mean frame (ms) | Reduction | Baseline → optimized p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| Game renderer, accurate/native sound; 5 pairs | 5.91440 | 4.32739 | 26.83% | 6.18144 → 4.49125 |
| FDP renderer, accurate/native sound; 5 pairs | 3.80996 | 3.60476 | 5.39% | 3.92846 → 3.73020 |
| Game renderer, existing HLE sound; 3 pairs | 4.24566 | 2.78605 | 34.38% | 4.48996 → 2.93561 |

Scale-4/border-48 composition: serial 59.7724 → 30.2417 ms (49.41% lower),
threaded 15.6151 → 7.96126 ms (49.02% lower). The fenced GPU/readback path was
0.956716 → 0.927828 ms; shaders did not change, so this small difference is not
claimed as a GPU optimization. Native emulation, native composition and scene
export in that harness measured 6.05075 → 4.39887 ms.

The final small candidates were measured independently against the preceding
optimized binaries: cached dense addresses reduced the FDP median by 0.63%;
opaque RGB cases reduced the native/game median by 0.94%. All raw pairs, including
one disturbed FDP run, remain in the local evidence. Those changes also affect
compiler layout; instruction count alone is not the acceptance criterion.

| Callgrind, identical 120-frame gameplay window | Baseline | Optimized | Reduction |
| --- | ---: | ---: | ---: |
| Instructions | 9,146,815,086 | 7,340,202,955 | 19.75% |
| Data references | 2,990,283,792 | 2,394,676,576 | 19.92% |
| Simulated I1 misses | 4,389,008 | 4,327,202 | 1.41% |
| Simulated D1 misses | 6,730,773 | 6,608,907 | 1.81% |
| Simulated branch mispredictions | 51,011,311 | 48,730,077 | 4.47% |

The compositor object's text increased from 15,304 to 53,714 bytes; this is the
cost of inlining and bounded scale specialization, not a whole-binary size
measurement. Callgrind reported its `brk segment overflow` limitation during
startup; the runs completed with matching frame/sample/cycle/block counts and
final state CRC `7bd46cbe`. Simulated counters are supporting evidence, not a
substitute for the repeated native timings.

### Reproducing the exercised paths

With a configured Release build and the supported ROM directory:

```sh
ctest --test-dir build/opt --output-on-failure
build/opt/f3rt-gameplay-regression --rom-dir roms/landmakrj --seed 5 \
  --frames 1320 --sound-driver native --video-diff --wav build/opt/replay.wav
build/opt/f3rt-gpu-regression --rom-dir roms/landmakrj --seed 5 \
  --frames 1560 --scale 4 --border 48 --every 120 --bench
build/opt/f3rt-netplay-oracle --mode sync-proof --rom-dir roms/landmakrj \
  --seed 5 --frames 2400 --sound-driver all
```

The ignored local probe binaries preserve the original and final executable
layouts. Replay a timing pair below, alternating order for five pairs as above;
substitute `fdp native` or `game hle` for the other paths. Do not rebuild the frozen
baseline archives against the optimized headers.

```sh
taskset -c 2 build/opt-baseline/probe-hle roms/landmakrj 3600 5 game native 1 0 bench
taskset -c 2 build/opt/probe4 roms/landmakrj 3600 5 game native 1 0 bench
valgrind --tool=callgrind --instr-atstart=no --cache-sim=yes --branch-sim=yes \
  --I1=32768,8,64 --D1=32768,8,64 --LL=134217728,2,64 \
  --callgrind-out-file=build/opt/replay.callgrind \
  build/opt/probe4 roms/landmakrj 1320 5 game native 1 0 bench
callgrind_annotate --auto=no build/opt/replay.callgrind
```

The probe starts instrumentation at frame 1200 and stops it before state hashing.
For a whole-program profile on another build, use the public gameplay regression
command with Callgrind's default instrumentation instead; do not compare its
startup-inclusive totals with this steady window. Memcheck uses the public
1320-frame native gameplay path with `--track-origins=yes --leak-check=full
--show-leak-kinds=all --error-exitcode=91`; the isolated-loader prerequisite on
this host is described below.

### Exercised correctness and remaining limits

- All five CTest checks passed, including permanent sparse/dense registration,
  failed replacement, reset/load, wide-bus boundary/wrap and DSP HALT/END/PC/delay
  regressions.
- Thirteen frozen-baseline/optimized stream comparisons matched: native/game
  seeds 1..8 at 6000 frames; FDP/native and HLE/game seed 5 at 6000; oracle/game
  seed 5 at 2400; expanded native/game at scale 3/border 160 and scale 4/border 48
  at 1560. Every-frame native/presentation pixel and PCM CRC streams, canonical
  state CRC streams every 120 frames, final state sizes/CRCs and execution/sample
  counts matched. HLE was compared with the original HLE path, not accurate audio.
- Real Vulkan layer/composite comparisons exercised scales 1..8 at border 160:
  43 composite samples, 33 supported layer samples (39 for sprite plane 3), zero
  mismatching pixels. Bitmap, trails, global flip, unknown producer, ending
  producer-boundary and sprite-boundary injections recovered exactly. The ending
  injection is not a played-through ending.
- Snapshot proof exercised native and oracle sound, frames 0/1000/2000/3000,
  replay depths 1/7/16/31/97: 40 exact checks including sound-trace records and
  wall-clock perturbations; zero save/load allocations. Final cross-presentation
  sync proof at 2400 frames passed 786 checks per sound driver, with exact local
  replay and zero save/load allocations in game and compare modes.
- Valgrind 3.25.1 Memcheck ran 1320 actual native gameplay frames with layer
  comparisons: zero errors, zero suppressions, zero blocks/bytes live at exit.
  The host's stripped CachyOS loader lacked required `memcmp` symbols and its
  debug build ID was unavailable from debuginfod. Only an ignored executable
  copy's ELF interpreter/RPATH was changed for this check, using locally extracted
  matching Arch glibc/debug 2.44+r50 packages; system libraries were not modified.
  Its frame, CPU and sample counts matched the normal runtime.
- The public ROM-access profiler built and executed native seed 5 for 1560
  frames. All fetch/data-read/descriptor intervals matched a real ordered-byte
  read oracle: main fetched 33,834 bytes, read 37,561, descriptor evidence 5480;
  sound fetched 21,268 and read 33,992. Wide access therefore retains diagnostic
  coverage, not merely output pixels.
- The actual Wayland frontend opened and rendered 1560 frames at scale 3/border
  48 on the CPU backend. The captured surface was inspected; late-candidate
  captures had zero differing pixels.

Local raw evidence is under `build/opt/final-benchmarks/`,
`build/opt/final-parity/`, `build/opt/final.callgrind`,
`build/opt/final-profile-comparison.json`, `build/opt/final-memcheck.log` and
`build/opt/access-profile/`; the frozen comparison archives remain under
`build/opt-baseline/`. These are ignored investigation outputs, not portable
checked-in fixtures.

Remaining profile cost is actual sampling/blending, DSP pipelines and ordered
CPU/device work. A larger direct dispatch map, mutable DSP decode caches,
reordered/batched device execution, packed arithmetic requiring additional
bounds assumptions, or GPU/HLE approximations would add memory, invalidation or
accuracy risk without demonstrated benefit here; none was retained. These
finite schedules preserve observed model behavior, not exhaustive game,
physical-bus timing or physical-board waveform accuracy.

### Second-pass generic candidates

Starting checkpoint `5d5e6ca` preserves the first verified exact pass. Subsequent
candidates use generic algorithms, not ROM/opcode/program special cases:

- `GameTiles::RowSampler` caches one normalized map-column's descriptor and tile
  row for a compose layer/subrow. Wrapping/reflection determines the key; arbitrary
  X jumps, repeats, mosaics and clip gaps need no sequential-X assumption. Cache
  lifetime ends with the subrow; it is not serialized. Reverse by restoring direct
  sampling and removing the factory/sampler plus its compositor use together.
- `GameSprites::raster` computes the sixteen possible clipped X columns once per
  sprite, retaining texel-row order, transparency, priority, trails and original
  fixed-point rounding. Reverse by moving X geometry back into the row loop.
- ES5510 `sext24`, `add` and skip-condition calculation use equivalent mask/bias,
  packed flag and Boolean arithmetic; operand reads and pipeline ordering stay
  unchanged. Reverse those three helpers/expressions independently.
- HLE synthesis caches pure encoded-gain lookup results, invalidated by every
  gain change; equal-current/target volume does not need a zero-delta ramp.
  HLE effects replace bounded cursor/remainder divisions with conditional wrapping.
  Float arithmetic ordering, sample rate and effects topology are unchanged.
  Reverse these in `hle_synth.cpp` and `hle_effects.hpp`; they are exact against the
  existing HLE output, not a claim of accurate-engine waveform equivalence.

Isolated five-pair accurate/native game measurements against this checkpoint:
DSP arithmetic 4.33096 → 4.29656 ms (0.79% lower); sprite columns
4.33152 → 4.24708 ms (1.95%); tile rows 4.34219 → 4.12845 ms (4.92%).
Combined: 4.33057 → 4.03624 ms (6.80%), p95 4.50515 → 4.17643 ms.
Three-pair controls: FDP/native 3.60966 → 3.58580 ms (0.66%);
game/HLE 2.78614 → 2.49021 ms (10.62%). These combined binaries precede the
separate exact HLE gain/effects candidate. All per-run observations remain in
`build/opt/round2-benchmarks/results.json`; no outliers were removed.

Generic synthetic smoke compared 2,097,152 cached/direct tile samples with an
independent descriptor formula, including negative/extreme coordinates, tile-ID
masking, all local/global flips, transparent/masked pens and palette/blend fields.
288 complete sprite-plane comparisons used the original raster loop across scales
1..8, borders 0/48/160, overlap, retained trails, fractional/offscreen positions,
and scales from 0 through 65535. All matched. A permanent device regression covers
row-cache boundary/wrap/reverse/repeated-coordinate transitions without ROM data.
Native and HLE 2400-frame pixel/PCM/state CRC streams and counts matched the frozen
checkpoint; HLE includes the later gain/effects candidate. These are fingerprint
comparisons, not a claim of direct byte comparison of every frame.

### Upstream integration and deferred native composition

`3ce8ee7` checkpoints the generic second pass. `bcfc122` merges upstream
`eb31e0f` (GPU presentation/native audio pacing decoupling) without conflicts.
For upstream comparison, core/frontend/GPU libraries were built from that exact
revision in an isolated worktree with the same compiler/flags and frozen generated
CPU archives. No profile-slim or firmware-specific specialization was introduced.

Five alternating pairs, CPU 2, seed 5, 3600 frames; means cover frames 1200–3599.
Setup, hashes, final snapshots, disk output and window/GPU draws are outside the
per-frame measurement. GPU rows measure strict native execution, audio drain and
semantic scene export with no native CPU pixel observer, **not GPU presentation**.

| Path | Upstream main mean | Merged exact mean | Lower frame time |
| --- | ---: | ---: | ---: |
| CPU Game / native audio | 5.88735 ms | 4.01973 ms | 31.72% |
| FDP / native audio | 3.81136 ms | 3.57673 ms | 6.16% |
| CPU Game / HLE48k | 4.23925 ms | 2.44628 ms | 42.29% |
| GPU scene export / native audio | 5.92308 ms | 2.55565 ms | 56.85% |
| GPU scene export / HLE48k | 4.28661 ms | 0.958188 ms | 77.65% |

The GPU rows use scale 3/border 48. Against the already optimized eager control,
deferred composition alone gives 4.06496 → 2.55565 ms (37.13%) for native audio
and 2.49053 → 0.958188 ms (61.53%) for HLE. Their p95 values are
4.18493 → 2.65824 ms and 2.63511 → 1.06032 ms. Every run matched main-machine
blocks/cycles, generated PCM counts, canonical size and final state CRC.
Raw paired records: `build/opt/main-comparison/results.json` and
`build/opt/lazy-benchmarks/results.json`.

Only supported GPU `Game` frames defer native composition. `native_pixels()`
reconstructs captured maps/glyphs/rows/palette with the retained current sprite
plane; it does not read the now-latched sprite list or live palette. Two fixed
native planes cost another 221,184 bytes; only the original next-frame plane is
serialized. CPU presentation and Diagnostic/Compare stay eager. Observations,
canonical/sync saves and backend/scale/load/reset transitions preserve the same
native output and state layout. Reverse this checkpoint as a unit: restore eager
native composition and the original public pixel field together with every caller.

Direct-byte smoke: 14,600 frames over four input seeds plus expanded presentation,
477 irregular native/state observations and 147 mutation/transition branches.
Native/expanded pixels, complete canonical/sync bytes and PCM matched an independent
CPU-eager peer. All GPU scales 1–8 matched the CPU reference; scale 3 additionally
exercised all nine layers, bitmap/trails/global flip guards,
sprite boundaries and runtime scale 1/8/3 changes. Durable `verify_deferred_native` covers ten first-observer and
pending-frame boundaries, with independent visible sprite-lag witnesses:

```sh
taskset -c 4-7 build/opt/f3rt-gpu-regression --rom-dir roms/landmakrj \
  --seed 5 --frames 1501 --every 1500 --scale 3 --border 48 \
  --inject-frame 1501 --inject-trails
taskset -c 4-7 build/opt/f3rt-netplay-oracle --rom-dir roms/landmakrj \
  --mode sync-proof --sound-driver native --frames 2400 --seed 89
```

The sync proof passed 786 checks, 372 paired frames and 372 exact local replays,
comparing 375,648 PCM samples; save/load allocations were zero. An actual Wayland/
Vulkan frontend under a nested 30 Hz Gamescope compositor ran 1200 frames on the
pre-main, upstream-main and merged-lazy executables. Clock resyncs were 108/0/0;
audio queue drops were zero for all three. Captured BMPs and WAVs were directly
byte-identical, native CRC `e8cc7573`, and the merged GPU surface was visually
inspected. These are real presentation observations, separate from the export
microbenchmark; native internal resolution was 320×232 and the window was 960×696. Evidence:
`build/opt/live-pacing/`.

Native GPU-export Callgrind ROI 1200–1319, with cache/branch simulation:
main 9,227,738,049 → exact 4,053,181,051 instructions (56.08% lower), data references
51.95% lower, D1 misses 31.14% lower, branch mispredicts 12.12% lower.
Raw files: `build/main-compare/native-gpu.callgrind-01`,
`build/opt/lazy-native.callgrind-01`, `build/opt/lazy-profile-comparison.json`.
Both completed with equal canonical state despite Valgrind's startup brk-segment
warning. The simulated LL cache is 128 MiB, not a claim about the host's 96 MiB L3.
Final `perf` on seed 89 collected 63,373 samples with zero lost samples:
ES5510 23.68%, audio scheduling 14.74%, main dispatch 8.88%, sprite raster 3.73%,
IRQ getter 3.72%. It includes startup, unlike the Callgrind ROI.

Separate exact HLE gain/effect refinements give 2.48744 → 2.44426 ms (1.74%) in
Game and 2.04674 → 1.99845 ms (2.36%) in FDP, five pairs each. Direct WAV/event
comparison matched. A generic all-program corpus covered 100 programs, seven
keys (30–108) and polyphony 1/8/32: 2100 cases, 1758 voice-allocating cases,
31,618 Starts and 259,485 ordered events. Before/after gain/effect float PCM
(40,204,800 eight-bus values), all event fields/order and 1,310,400 stereo effect
frames across 13 presets/ring wraps matched byte-for-byte. Six existing rejected
cases (programs 81/82, key 39, each polyphony) retained the same external-directory
error; they are not successful synthesis cases. Evidence:
`build/hle-native-rate/corpus/` and `build/opt/round2-hle-bench/results.json`.

`e5a87e5` checkpoints the exact deferred-native/accessor cutover and durable
regressions. Final CTest passed 5/5. Targeted Memcheck loaded a ready real snapshot,
executed 40 supported unobserved scanouts and ten pending-frame/mutation/fallback/
backend/scale/reset/load/trail boundaries: native/expanded/canonical/sync bytes
matched, zero errors and zero bytes live at exit (174 allocations/frees). It used
an isolated matching glibc/debug loader because the host loader lacks Valgrind's
required redirect symbol. Raw result: `build/opt/lazy-targeted-memcheck.log`.
The earlier full 1800-frame Memcheck attempt exceeded its command deadline;
this is targeted retention-path coverage, not a completed full-run Memcheck.

The merged temporal GPU regression also completed seed 5 / scale 2 / 4000 frames:
46 samples, 43 paired scenes, 30 visible intermediate/midpoint frames, exact
alpha-1/repeated draws, canonical/native/audio parity, and exact reset/gap/
duplicate/rollback/fallback replay. Captured midpoint output was visually
inspected. Reproduce with `build/opt/f3rt-motion-regression --rom-dir
roms/landmakrj --frames 4000 --seed 5 --scale 2 --every 90 --interp off`;
PNG evidence remains in `build/opt/lazy-motion-proof/`.

Two final generic exact candidates were measured and rejected rather than retained:

| Candidate | Exact control | Candidate | Decision |
| --- | ---: | ---: | --- |
| ES5510 fixed decoded-instruction cache | 2.54036 ms | 2.57105 ms | 1.21% slower; all five pairs regressed |
| HLE filter stage/mode specialization | 0.953972 ms | 0.953158 ms | 0.09% median change, within run variation |

The decode cache required another 1920 bytes and mutable-program/reference
invalidation; the existing device checks passed, but that complexity did not buy
speed. Specialized filtering preserved the full all-program float/event corpus;
no stable whole-runtime gain justified replacing the simpler loop. Records:
`build/opt/final-candidates/results.json`. Remaining sampled native work is mainly
required DSP/sound CPU execution, scheduling, dispatch and exact sprite work;
no additional instruction/timing bypass or speculative state cache was retained.

### Final native-audio pass

Starting from `287ce51`, two source-only changes preserve the current device
model, operation order, API and canonical state layout:

- ES5510 `execute_run` uses `[[gnu::flatten]]` to inline its per-instruction
  helpers. The emitted kernel has no ALU/register-helper calls or stack-canary
  reference. Object text grows from 18,446 to 25,519 bytes; no decode cache,
  allocations, pipeline-field omissions or instruction specialization is added.
- ES5505 `generate_pcm` shares the forward/reverse sample-read, interpolation,
  filtering and stereo-mix path. Only accumulator update and end-check selection
  depend on direction. Adjacent word addressing wraps at 20 bits before banking;
  stopped-voice masking, filter arithmetic, voice/IRQ order and callbacks remain.
  This is a source simplification, not a separately demonstrated speedup.

Five alternating runs per binary, CPU 2, seed 5, 3600 frames, measuring frames
1200–3599: isolated DSP inlining lowers median frame mean from 2.55417 to
2.40381 ms (5.89%) in native GPU semantic export. This excludes GPU drawing and
native pixel observations. Every run matches state CRC/size, cycles, blocks and
PCM counts. Isolated PCM deduplication differs by only 0.08%, within variation.
Unsigned CPU-deadline division, outlined PCM generation and their combination
were rejected (respectively 0.72%, 0.02% and 0.19% slower median frame means).
The additional oracle sprite-column experiment was stopped and removed when
wrap-up was requested; no partial benchmark result is treated as evidence.

For both retained changes, Callgrind seed 89 / ROI 1200–1319 gives
4,060,011,974 → 3,869,289,331 instructions (4.70% lower), data references 8.21%
lower, I1 misses 4.11% lower and branch mispredicts 2.15% lower. Simulated D1
misses increase 4.11%; the 128 MiB simulated LL is not the host's 96 MiB L3.
Both runs finish at state CRC `4b0f7960` with identical counts, despite the same
startup `brk segment overflow` warning. Records: `build/opt/round3/isolated-results.json`
and `build/opt/round3/callgrind-comparison.json`.

An independent old-object/current-object device smoke compares 12,288 synthetic
OTIS states × four frames and 2048 generic DSP programs × three samples. Direct
PCM/serial, complete pipeline/DRAM/canonical bytes and ordered IRQ events match.
It covers all voice counts, directions/loop/stop/filter modes, absent and wrapped
ROMs/banks, fractional addresses, volumes, all ALU/operand/RAM modes, special
registers, skip/HALT/END/PC wrap and reset/load transitions. CTest passes 5/5.
Actual native gameplay seed 89 / 1560 frames matches WAV and BMP bytes directly
(787,650 audio frames, native CRC `6ddd781b`); the capture was visually inspected.
Vulkan seed 5 / 1501 frames has ten exact composite samples, ten deferred-state
boundaries and exact induced trails fallback recovery.
Memcheck completed the retained `f3rt-check` device/boundary suite with zero
errors, zero suppressions, 617 allocations/frees and no live bytes or leaks.
It used the same isolated matching glibc/debug loader, without altering the host
or original executable. Record: `build/opt/round3/runtime-memcheck.log`.
The complete synthetic corpus passed natively; its full Memcheck run exceeded
180 seconds and is not a completed corpus-wide memory-check claim. Raw
observations and binaries remain under `build/opt/round3/`.

