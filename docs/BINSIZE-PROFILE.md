# Execution-profile binary-size experiment

## Decision

**Use full-coverage tiers, not the slim build, for a playable size reduction.**

- A: hot native units `-O2`, cold units `-Oz`; **all 1,295,420 original main/sound dispatch entries retained**, no fallback. File **62,699,592 bytes**, `__TEXT` **31,473,664 bytes**: 25.616% / 44.560% smaller than the supplied reference. All required parity gates passed.
- B: explicit `F3_PROFILE_SLIM=<profile>` retains 37,665 observed entries. File **6,003,528 bytes**, `__TEXT` **4,571,136 bytes**. Training and attract gates passed, but **7/8 held-out seeds aborted (87.5%)**. Rejected as a general-playability cutover; remains an opt-in experiment with loud, recorded cold hits.
- Human data is genuine but partial: user-confirmed single-player campaign, retained periodic snapshot; output-write failure lost the unflushed tail. No claim of completed matches, endings or exhaustive gameplay.

Worktree `wt/binsize-profile`, branch `binsize-profile`. Darwin 25.2.0 / arm64, Apple clang 21.0.0 (`clang-2100.1.1.101`). Japanese main ROM CRC32 `15a59a08`, sound CRC32 `5a7e9117`. No pushes, ROM bytes, binaries or generated code committed.

## Instrumentation and storage

`F3_PROFILE_INSTRUMENT=ON` regenerates both CPUs and compiles allocation-free, saturating `uint64_t` increments. `--profile-out FILE` activates dense per-word counters (10 MiB for these two program regions). No hot-path hashing, allocation or synchronization. Disabled macros compile away; noninstrumented slim miss recording does not allocate the dense counter arrays.

Main instrumentation is at every actual label, including intra-page fallthrough and entries whose hook redirects/stops execution. Shared main/sound exception handlers record the actual `cpu->pc` exactly once. Counters are outside canonical CPU/machine/snapshot state.

Version-1 ASCII format:

```text
F3-BLOCK-PROFILE 1
rom main 15a59a08 00000000 00200000
rom sound 5a7e9117 00c00000 00080000
hit main 15a59a08 ADDRESS8HEX COUNT_DECIMAL
miss sound 5a7e9117 ADDRESS8HEX COUNT_DECIMAL
```

The illustrated `ADDRESS8HEX` / `COUNT_DECIMAL` fields describe the format, not literal profile rows. Identities carry CRC32, base and size; rows contain addresses/counts only. Merging unions identities and addresses and adds counts with saturation. Generation validates version/CRC/bounds; `miss` rows do not become hot code.

Sequential runs merge into an existing destination. An advisory lock rejects simultaneous writers to the same path; independent processes use separate files and merge later. Every approximately 30 wall-clock seconds at a frame boundary and on normal exit, the writer flushes/fsyncs a temporary file and atomically replaces the destination. Relative paths are fixed against startup cwd. Explicit flush errors terminate the command; destructor errors are reported without discarding an earlier snapshot. Writable storage and free disk space are required.

Committed `profiles/landmakrj.profile`: **1,186,177 bytes**, SHA256 `5ae3c7b6e01b07a569ab984a7409cce33ab13282090435188ad65cb61ccffebb`. It contains no ROM bytes.

### Persistence proofs

- Two deterministic 600-frame runs into one file exactly doubled all 4,425 entry counts (main 1,467, sound 2,958).
- An independent headless process was SIGKILLed after 35.051 seconds. Its last periodic snapshot retained 18,561 keys / 272,208,868 hits. No human window was stopped by the collector.
- Concurrent writer exclusion was exercised.
- A real-runtime driver reproduced the old relative-path flush failure after changing cwd; the permanent C++ regression now survives cwd changes and preserves merged counts across sessions.
- A redirecting-hook regression proves that an entered block is counted even when its guest instruction is skipped.

## Collection provenance

Training: native seeded-mash harness, seeds **101–108**, **20,000 frames each**. Every training WAV and all seven final state files matched the supplied native reference byte for byte; zero main fallback. Collection files are `build/evidence/training/seed-N.profile`.

The first window ran 8m22s / 29,428 frames and exited normally. The user states they never saw or played it. `build/evidence/human.profile` is therefore **unattended attract data**, not a human match. Its command was:

```sh
./build/landmakr --profile-out build/evidence/human.profile --video-scale 2
```

The second window was visibly foregrounded; the user confirms playing the single-player campaign for a while. Exact command:

```sh
./build/landmakr --profile-out build/evidence/human-play.profile --video-scale 2
```

Working directory: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile`.
Profile: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile/build/evidence/human-play.profile`.
Service `HumanProfilePlay2`, pid 74901. Native sound and default CPU game video; no frame limit or collector early stop. Ignored desktop evidence: `build/evidence/human-play-visible.png`.

That process exited 1 after `Block profile write failed: build/evidence/human-play.profile.tmp`, rather than a normal Escape completion. Its valid periodic snapshot contains **25,244 keys** (main 17,858, sound 7,386), **266,141,218 hits**, and adds **9 previously unseen main entries** to the prior training/attract union. Exact final frame count and unflushed tail are unavailable. Disk was nearly full; a cwd-relative write defect was separately reproduced and fixed. The original window's precise failure cause was not captured with errno and is not proven. Write/open errors now include errno.

The frozen profile merges the eight training runs, the unattended window, a 3,480-frame instrumented attract capture, and the genuine campaign periodic snapshot. Held-out seeds 301–308, deliberate cold probes and crash-preservation data are **excluded**. Held-out failures were not fed back into training.

### Ever-executed entries by descriptive region

| Region | Original entries | Ever executed | Coverage | A retained | B retained |
|---|---:|---:|---:|---:|---:|
| Main code bin | 290,024 | 28,817 | 9.9361% | 290,024 | 28,817 |
| Main homogeneous padding | 486,966 | 0 | 0% | 486,966 | 0 |
| Main data/gfx-looking bin | 256,286 | 1,329 | 0.5186% | 256,286 | 1,329 |
| Sound program | 262,144 | 7,519 | 2.8683% | 262,144 | 7,519 |
| **Total** | **1,295,420** | **37,665** | | **1,295,420** | **37,665** |

Main total: 30,146 / 1,033,276 = 2.9175%. Main registrations comprise 464,523 decoded entries plus 568,753 proven exception entries. Sound registers every aligned word in the complete 512 KiB region.

Bins are descriptive, never discovery exclusions. Padding is measured homogeneous-fill ranges `[0x7030,0x10000)` and `[0x11b362,0x1ffffe)`. Gfx-looking is the zero-heavy `[0x10000,0x90000)` range; **1,329 entries there actually execute**, so entropy is not a non-code proof.

## A: full-coverage compile tiers

`F3_PROFILE_TIERS=<profile>` splits existing main pages into hot/cold subsets and emits separate units. A successor in the other subset flushes lazy flags and returns to dispatch; instruction/event boundaries stay intact. Sound single-instruction functions split into hot/cold shards without changing the dense table. Shared exception vectors have separate units: a vector is hot if any observed entry aliases it, otherwise cold. No duplicate helper bodies or pruned entries.

CMake applies `-O2` to hot units and `-Oz` on Clang (`-Os` on other supported compilers) to cold units. Effective compiler commands verified both `exceptions_cold.c` and `sound_exceptions_cold.c` at `-Oz`; registration/table units remain `-O2`. Runtime/shared code keeps ordinary Release options. The supplied reference and fresh ordinary full build use Release **`-O3`**, not `-O2`; the size comparison includes the requested hot optimization change as well as cold sizing/layout.

Actual cold NOP entries main `0x00003902` and sound `0x00c1330c` executed natively with expected +2/+4 instruction cycles and no fallback. Newly separated, actual `-Oz` cold line-F handlers were also exercised from main `0x00120000` and sound `0x00c40000`: main exception cost 20 cycles, sound 34; no interpreter fallback.

Full coverage means the existing baseline registration set is retained, not a claim that every baseline unlowered/unsupported path is implemented. Original unlowered main entries (3,855) and sound unsupported words (944) were never reached in the required gates. No newly removed path exists in A.

## B: opt-in removal and failure behavior

`F3_PROFILE_SLIM=<profile>` emits only observed executable statements and dispatch entries, not a full table of cold stubs. Main/sound retain 30,146 / 7,519 entries and remove 1,003,130 / 254,625 respectively. Main missing entries abort before fallback, even if a caller enables `allow_main_fallback`. Sound uses a validated sorted sparse table and binary search; it never switches to the oracle. Frontend and seeded harness reject interpreter/oracle/fallback operation in slim mode.

Example observed failure:

```text
F3 PROFILE SLIM ABORT: removed sound block address=0x00c1770a ROM_CRC=0x5a7e9117; re-profile with a full -DF3_PROFILE_INSTRUMENT=ON build and --profile-out FILE
```

The abort is nonreturning. It prints to stderr and appends/fsyncs a `.cold-hits` log, or `f3-cold-hits.log` without `--profile-out`. With a profile destination it also immediately flushes a structured `miss` record. Storage failures are reported; they never enable fallback or resume execution.

Real probes of both removed NOP entries aborted before changing the opcode PC/cycle state; zero fallback even with main fallback deliberately enabled. Both durable text records and structured misses were verified. Noninstrumented miss recording preserved every previous hit/count in a copied full profile.

### Held-out seeds: rejected availability

Each run requested 20,000 frames. Seven stopped at the following reported frames; only seed 306 completed, with WAV and all seven final state files byte-identical to reference.

| Seed | Result | Reported abort frame | CPU / missing address | CRC |
|---|---|---:|---|---|
| 301 | cold abort | 1,680 | sound `0x00c1770a` | `5a7e9117` |
| 302 | cold abort | 6,980 | main `0x00098282` | `15a59a08` |
| 303 | cold abort | 1,680 | sound `0x00c1770a` | `5a7e9117` |
| 304 | cold abort | 2,718 | main `0x000d7a66` | `15a59a08` |
| 305 | cold abort | 3,567 | main `0x000d7dd2` | `15a59a08` |
| 306 | 20,000 frames, WAV/state exact | — | — | — |
| 307 | cold abort | 1,730 | main `0x00006b6c` | `15a59a08` |
| 308 | cold abort | 2,896 | main `0x000f1cd0` | `15a59a08` |

**Abort rate 7/8 = 87.5%.** All abort logs report zero main fallback. This is observed risk, not an estimated lifetime failure probability. No wrong-output workaround, missing-address special case, retry or interpreter masking was added.

## Binary, source and timing results

All sizes are bytes. `__TEXT` is Mach-O segment size; `__text` is instruction-section size. File size includes tables, symbols and other segments.

| Build | File | `__TEXT` | `__text` | Generated main C | Generated sound C | Total C |
|---|---:|---:|---:|---:|---:|---:|
| Supplied reference | 84,291,656 | 56,770,560 | 54,706,200 | 261,098,018 | 55,210,095 | 316,308,113 |
| Fresh ordinary full | 84,458,008 | 56,918,016 | 54,746,656 | 278,754,410 | 58,852,878 | 337,607,288 |
| Instrumented full | 105,856,488 | 78,102,528 | 75,920,552 | 278,754,410 | 58,852,878 | 337,607,288 |
| A: full tiers | 62,699,592 | 31,473,664 | 29,555,092 | 277,678,833 | 58,853,303 | 336,532,136 |
| B: slim | 6,003,528 | 4,571,136 | 4,309,200 | 15,549,645 | 3,569,652 | 19,119,297 |

Ordinary generated C includes the compile-away profiling macro text, explaining source growth versus the older supplied reference. Against the fresh ordinary full build, A reduces file / `__TEXT` by **25.762% / 44.704%**; B by **92.892% / 91.969%**.

### Generated C attribution

| Region | Reference | Fresh full | A tiers | B slim |
|---|---:|---:|---:|---:|
| Main code | 119,070,273 | 127,836,937 | 126,832,575 | 14,909,255 |
| Main padding | 20,452,572 | 20,452,572 | 20,452,572 | 0 |
| Main gfx-looking | 121,541,115 | 130,426,427 | 130,349,560 | 635,117 |
| Sound program | 55,198,307 | 58,838,021 | 58,838,021 | 3,568,131 |
| Generated shared overhead | 45,846 | 53,331 | 59,408 | 6,794 |

Attribution scans only active filenames in generation manifests; stale files are excluded.

### Native `__text` function-span attribution

| Region | Reference | Fresh full | A tiers | B slim |
|---|---:|---:|---:|---:|
| Main code | 22,104,880 | 22,104,880 | 11,778,248 | 2,758,424 |
| Main gfx-looking | 22,214,256 | 22,214,256 | 11,667,960 | 120,204 |
| Sound program | 9,616,948 | 9,616,948 | 4,413,372 | 620,804 |
| Runtime/shared/outlining/alignment | 770,116 | 810,572 | 1,695,512 | 809,768 |

These are symbol spans, including alignment/page packing. Outlined helpers and shared exception code cannot be assigned uniquely to guest regions. Padding entries share exception bodies; their substantial cost is the dispatch table, not independent native function text. `__TEXT` non-instruction overhead is separate. No exact guest-region attribution of whole-file bytes is claimed.

Dispatch table bytes (16 bytes per entry on this arm64 build):

| Region | Full/reference/A | B slim |
|---|---:|---:|
| Main code | 4,640,384 | 461,072 |
| Main padding | 7,791,456 | 0 |
| Main gfx-looking | 4,100,576 | 21,264 |
| Sound program | 4,194,304 | 120,304 |

### Configure/build wall time

Seconds, `-j 6`, targets `landmakr` and `f3rt-gameplay-regression` (full/instrumentation also included `f3rt-check`). Initial clean builds include runtime/third-party compilation; final regeneration builds reuse those objects. Concurrent host load and nearly-full-disk recovery make these measurements non-isolated benchmarks. Supplied reference's original configure/build timing was not recorded.

| Build | Initial configure | Initial build | Final configure | Final regeneration/build |
|---|---:|---:|---:|---:|
| Fresh full | 28.639 | 129.318 | 14.913 | 47.911 |
| Instrumentation | 16.780 | 117.120 | 22.798 | 98.180 |
| A tiers | 40.716 | 112.848 | 14.851 | 48.650 |
| B slim | 15.219 | 16.430 | 6.587 | 3.948 |

Final A timing includes the independently tiered shared exception units. Instrumentation also had an earlier saturation-update rebuild of 232.58 seconds including configure.

Regional compiler durations below are **sums of latest active-object Ninja elapsed times**, not build wall time or CPU time. Main objects with native symbols exclusively in one bin are classified there; mixed shards, tables, registration and shared exceptions stay separate. Sound objects are grouped together. Configure/discovery time is whole-build only; shared/mixed units prevent a defensible exact per-region wall-time decomposition.

| Object group | Fresh full | A tiers | B slim |
|---|---:|---:|---:|
| Pure main code | 108.874 | 115.080 | 12.788 |
| Pure main gfx-looking | 113.147 | 103.900 | — (mixed units) |
| Main mixed/shared/dispatch | 5.972 | 8.104 | 1.837 |
| Sound | 44.881 | 41.507 | 3.424 |
| Independent padding units | none | none | none |

## Correctness gates

Fresh MAME capture used a separate ROM staging directory, cfg and NVRAM, not a stale reference dump. Frames **600 through 3,480, step 120**, 25 frames / 1,856,000 pixels per comparison. Undumped PAL / reduced-subtarget parent warnings were expected; main/sound ROM chips were CRC-validated.

| Build | Fresh MAME frames | RGB mismatches | Main RAM600 | Attract WAV vs native reference | Seeds 101–108 × 20,000 | Seeded WAV + final state |
|---|---:|---:|---|---|---|---|
| Supplied reference | 25/25 | 0 | exact | reference | 8/8 | reference |
| Fresh full | 25/25 | 0 | exact | exact | 8/8, zero fallback | 8/8 exact |
| Instrumented full | 25/25 | 0 | exact | exact | 8/8, zero fallback | 8/8 exact |
| A tiers | 25/25 | 0 | exact | exact | 8/8, zero fallback | 8/8 exact |
| B slim | 25/25 | 0 | exact | exact | 8/8, zero fallback | 8/8 exact |

Final state files compared at frame 20,000: `palette.bin`, `graphics.bin`, `control.bin`, `mainram.bin`, `shared.bin`, `rendered.argb`, `cpu.json`. Native sound was selected for every target run. Reference held-out seeds 301–308 all completed 20,000 frames; slim held-out seed 306 matched WAV/state, the other seven aborted before completion. Eight trained seeds passing is not held-out admission.

22 unique Python/compiled-C/C++ regressions passed: discovery/overlap/deadlines, profile identity/bounds/saturation, cross-tier fallthrough/indirect entry, redirected hooks, both CPUs' shared exceptions, sparse removal and cwd-stable runtime persistence. Actual runtime `f3rt-check`: `PASS memory/lanes, input/coin, EEPROM protocol, IRQ/stack, native dispatch and real interpreter` (the diagnostic interpreter check is not fallback in any candidate game run).

The final fixed instrumentation frontend also ran with `--profile-out`, passed 25/25 / RAM600 / WAV, and wrote a real profile. This post-fix smoke was not merged into the frozen training corpus.

## Reproduction

Run from this worktree, with the locally supplied ROM directory. Separate directories keep modes isolated:

```sh
export PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages
ROM=/Users/darien/Workspace/f3-stuff/roms/landmakr
PROFILE="$PWD/profiles/landmakrj.profile"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR="$ROM" -DF3_PROFILE_INSTRUMENT=ON
cmake --build build --target landmakr f3rt-gameplay-regression -j 6
./build/landmakr --profile-out build/evidence/human-play.profile --video-scale 2

cmake -S . -B build/tiers -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR="$ROM" -DF3_PROFILE_TIERS="$PROFILE"
cmake --build build/tiers --target landmakr f3rt-gameplay-regression -j 6
cmake -S . -B build/slim -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR="$ROM" -DF3_PROFILE_SLIM="$PROFILE"
cmake --build build/slim --target landmakr f3rt-gameplay-regression -j 6
```

Training invocation, repeated for seeds 101–108 with independent destinations:

```sh
./build/f3rt-gameplay-regression --rom-dir "$ROM" --sound-driver native --seed 101 --frames 20000 --wav build/evidence/training/seed-101.wav --dump-dir build/evidence/training/seed-101 --profile-out build/evidence/training/seed-101.profile
python3 tools/block_profile.py merge --output profiles/landmakrj.profile build/evidence/training/seed-{101,102,103,104,105,106,107,108}.profile build/evidence/human.profile build/evidence/instrument-attract.profile build/evidence/human-play.profile
```

Seeded harness is inherently headless/unthrottled. For candidate gates use the corresponding `build/full`, `build/tiers` or `build/slim` executable and omit `--profile-out`; test held-outs 301–308 without merging them. Attract/report example:

```sh
./build/tiers/landmakr --headless --unthrottled --frames 3480 --dump-dir build/evidence/tiers-final-attract --dump-start 600 --dump-every 120 --wav build/evidence/tiers-final-attract.wav
python3 tools/compare_frames.py build/evidence/mame-attract build/evidence/tiers-final-attract --json --quiet
python3 tools/block_profile.py report profiles/landmakrj.profile --main-generated build/tiers/generated/landmakrj --sound-generated build/tiers/generated/sound-landmakrj --binary build/tiers/landmakr
```

Local ignored evidence: `build/evidence/final-gates.json`, `*-final-sizes.json`, `heldout-cold-hits.json`, `regional-compile-times.json`, configure/build command JSON/logs, parity/state files, preserved profiles and screenshots. Candidate/training duplicate WAVs were discarded only after exact comparison; hashes and golden reference WAVs remain. An early parallel gate batch ran out of disk while writing evidence; inconclusive gates were rerun after removing only experiment-generated C and verified duplicate WAVs. Two already-observed cold-abort stderr logs were retained despite lost exit-code JSON, not rerun or disguised as I/O failures.

Generated C was measured then removed to recover space; **rerun CMake configure before rebuilding those directories or recomputing source attribution**. Binaries, objects, inventories and measured JSON remain local. Throwaway smoke sources/executables were removed after successful execution. No shared/user artifacts were deleted.

Tags: `binsize-profile-1-instrumentation`, `binsize-profile-2-collected`, `binsize-profile-3-tiers`. Profiles are finite coverage evidence, not a proof of unreachable cold code. A avoids that risk by retaining it; B demonstrably cannot replace A with this corpus.
