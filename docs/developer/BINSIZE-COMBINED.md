# Combined binary-size cutover

## Decision and scope

Default ROM-generated `landmakrj`: the six reviewed config exclusions plus
**full-coverage hot `-O2` / cold `-Oz` tiers** (`-Os` outside Clang).
No additional removal, cold stubs or interpreter coverage workaround.
`F3_PROFILE_SLIM` remains explicit cold removal, rejected for general play.
The combined-build checkpoint is summarized in [validation scope](VALIDATION.md).

Integration/GPU/automatic-scale/audio-stall behavior is retained. Main and sound
require **ABI 3**, not profile experiment B's historical ABI 2. CPU layout,
timing/deadlines, ROM data reads and canonical snapshot contracts are unchanged.
No pushes; ROMs, generated C, executables and captures remain uncommitted.

## Configure and run

```sh
export PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages
ROM=/Users/darien/Workspace/f3-stuff/roms/landmakr
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DF3_ROM_DIR="$ROM"
cmake --build build --target landmakr f3rt-gameplay-regression -j 6
./build/landmakr --video-backend gpu --video-scale auto-integer --video-border 48

# Ordinary Release optimization; the same config exclusions remain on.
cmake -S . -B build/plain -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_ROM_DIR="$ROM" -DF3_PROFILE_DEFAULT_TIERS=OFF -DF3_PROFILE_TIERS=
# Explicit full-coverage profile override.
cmake -S . -B build/custom -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_ROM_DIR="$ROM" -DF3_PROFILE_TIERS="$PWD/profiles/landmakrj.profile"
# Explicit removal experiment, not the default or a general-playability recommendation.
cmake -S . -B build/slim -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DF3_ROM_DIR="$ROM" -DF3_PROFILE_SLIM="$PWD/profiles/landmakrj.profile"
cmake --build build/slim --target landmakr f3rt-gameplay-regression -j 6
```

`F3_PROFILE_DEFAULT_TIERS=ON` selects the frozen profile only with ROM generation
and no explicit tiers/slim override. The cache's `F3_PROFILE_TIERS` value stays
empty for this automatic selection. An explicit cached profile must be cleared
when opting out. Tiers/slim overrides are mutually exclusive; their profiles
are configure dependencies. Pre-generated-only builds keep their supplied mode.
The opt-out was configured, built and actually run for 600 native frames:
336,775 main / 61,997 sound registrations, zero fallback.

## Semantic merge and exclusion precedence

The same six half-open intervals remain applied:

| CPU | Interval | Evidence |
|---|---|---|
| Main | `0x007030..0x010000` | Exact FF fill |
| Main | `0x020000..0x088000` | Reviewed graphics/script-table bank; rooted/native-fetch and gameplay evidence |
| Main | `0x11b362..0x1ffffe` | Exact FF tail; final checksum word retained |
| Sound | `0xc1e45a..0xc20000` | Exact FF fill |
| Sound | `0xc20000..0xc39a30` | Reviewed sequence/header bank; native-fetch/WAV evidence |
| Sound | `0xc39a30..0xc80000` | Exact FF tail including unpopulated chip halves |

Full tiers partition **336,775 main / 61,997 sound** post-exclusion entries:
main hot/cold **30,146 / 306,629**, sound hot/cold **7,519 / 54,478**.
Main pages split only their filtered labels. Cross-tier successors flush lazy
flags and return to dispatch at the existing instruction/event boundary.
Sound hot/cold shards preserve the exact dense exclusion complement.
Shared exception bodies are emitted once, with a hot helper only when a retained
observed entry aliases it. Effective commands: **29 hot `-O2` units / 128 cold
`-Oz` units**, including **1 hot / 2 cold shared-exception units**.

ABI 3 immutable exclusion spans are registered in tier and slim programs.
Full sound dispatch subtracts preceding excluded words; no second map is
allocated. Slim validates a sorted nonexcluded sparse subset and binary-searches
it. Both CPUs reject excluded physical targets before fallback or slim cold-miss
handling, including odd PCs, trace-mode main execution and upper-byte aliases.
The main slim rejection path explicitly preserves the existing ABI 3 guard.

Real-ROM probes on **each** of full tiers and slim: **48 main + 12 sound**
excluded-start/odd/end-minus-one/alias checks, main strict/allow-fallback and
trace/nontrace combinations, **3 main half-open-end checks**, zero main fallback
and zero sound instruction progress at excluded targets. Full-tier main
`0x3902` and sound `0xc1330c` cold NOPs execute with +2/+4 cycles. Explicit slim
misses at those same nonexcluded addresses throw and durably record CPU/address/
CRC/re-profile diagnostics; the probe catches them and exits 2, while the
frontend's error handler exits 1. No exclusion becomes a cold-hit record.

The first combined configure exposed an unintended widened sound-vector seed
policy: upper-byte normalization treated ROM slot 136's `0xffc48080` as a new
excluded root. The merge restores the exclusion experiment's existing canonical
seed policy; runtime physical-alias exclusion enforcement remains strict.
No exclusion or ROM was changed to hide that configure failure. Evidence retains
the failed log and corrected successful configure.

## Frozen profile and provenance

`profiles/landmakrj.profile`: **1,186,177 bytes**, main CRC `15a59a08`, sound CRC
`5a7e9117`, SHA256
`5ae3c7b6e01b07a569ab984a7409cce33ab13282090435188ad65cb61ccffebb`.
Only identities, addresses and counts; no ROM bytes.

The original corpus was re-merged into an ignored `refrozen.profile` and audited
against all six current intervals. It is **byte-identical** to the committed
profile: **30,146 main / 7,519 sound hit keys**, **0 misses**, **0 excluded
records**, no dropped hot entries. CRC/base/size still identify the full images,
not a trimmed byte image. Both generators now reject matching hit **or miss**
records inside an exclusion with CPU/address/range/reason diagnostics before
partitioning; silently ignoring contradictory execution would hide a bad
exclusion.

Inputs retained exactly:

- Eight native seeded runs, **101–108 × 20,000 frames**.
- The first unattended attract window, explicitly **not human play**.
- The original 3,480-frame instrumented attract capture.
- The genuine user-confirmed single-player campaign periodic snapshot:
  25,244 keys / 266,141,218 hits, adding 9 main entries to the prior union.
  That window exited on a profile-output write failure; its unflushed tail and
  final frame count remain unknown. No completed-match or ending claim.

Held-outs **301–308**, deliberate cold probes and crash-preservation probes were
not merged. The combined human window is noninstrumented and does not retrain
these gate seeds. Original commands and detailed human provenance remain in
[BINSIZE-PROFILE.md](BINSIZE-PROFILE.md); per-input hashes/counts and the exact
refreeze audit are in ignored `build/combined-evidence/profile-audit.json`.

## Size and configure/build table

Darwin arm64, AppleClang 21 Release. All sizes are **bytes**; `__TEXT` is the
Mach-O segment, not just the `__text` instruction section. Objects are active
generated `.c.o` files, excluding runtime objects and archive container bytes.

| Build | File | `__TEXT` | Generated C | Generated objects | Registered main / sound | Configure / build seconds |
|---|---:|---:|---:|---:|---:|---:|
| Baseline, no techniques | 84,436,984 | 56,901,632 | 316,308,113 | 105,939,312 | 1,033,276 / 262,144 | 14.864 / 54.537 |
| A only: exclusions | 42,778,072 | 32,604,160 | 164,220,955 | 52,023,360 | 336,775 / 61,997 | 7.844 / 26.999 |
| B-tier only: no exclusions | 62,699,592 | 31,473,664 | 336,532,136 | 101,780,736 | 1,033,276 / 262,144 | 14.851 / 48.650 |
| **Combined full coverage** | **31,525,032** | **18,857,984** | **175,404,686** | **50,629,824** | **336,775 / 61,997** | **8.028 / 27.619** |
| Combined + explicit slim | 6,023,976 | 4,587,520 | 19,120,803 | 6,122,760 | 30,146 / 7,519 | 3.520 / 6.086 |

Combined file / `__TEXT`: **62.664% / 66.859% smaller** than the table baseline.
File is **26.306% smaller than A only**, **49.721% smaller than B-tier only**.
Generated main/sound units: 105 / 52 (157 objects); slim 19 / 9 (28 objects).
Main packed native functions: 13,131 full / 2,256 slim; sound emitted functions
including shared vectors: 49,423 full / 7,497 slim. Entry counts, not these host
function counts, express coverage.

The first three rows retain the two completed experiments' measured provenance;
they are **not five fresh same-tree isolated benchmarks**. Baseline/A-only data
come from the GPU-enabled exclusion worktree; B-tier uses its pre-exclusion
worktree and earlier frontend. B objects were independently summed against its
252 active manifest entries. New combined/slim rows are actual successful
configure/builds in this worktree, targets `landmakr` and gameplay regression,
`-j 6`; other callers built separately in 1.498 s. B's final build reused runtime
objects; combined/slim include fresh runtime compilation. Concurrent host work
makes times non-isolated. The baseline/A Release optimization was `-O3`; tiers
also change hot compilation to the requested `-O2`. Profiling macros compile
away in production but increase generated source bytes versus experiment A.

The parity reference is separately the supplied
`/Users/darien/Workspace/f3-stuff/f3-recomp/build/landmakr`, whose historical file
size is 84,291,656 bytes; do not confuse it with the fresh A size baseline.

### Per-region accounting

Same descriptive bins as `tools/block_profile.py`; these are not new exclusions.
The broad gfx-looking bin includes **real executed code** outside the selected
`0x020000..0x088000` data interval.

| Region | B-tier entries → combined | B-tier C → combined C | B-tier native symbol spans → combined |
|---|---:|---:|---:|
| Main code bin | 290,024 → 290,024 | 126,832,575 → 126,832,758 | 11,778,248 → 11,778,248 |
| Main homogeneous padding | 486,966 → 0 | 20,452,572 → 350 | Shared exceptions, no independent functions |
| Main gfx-looking bin | 256,286 → 46,751 | 130,349,560 → 22,464,482 | 11,667,960 → 2,031,676 |
| Sound program/data | 262,144 → 61,997 | 58,838,021 → 26,068,904 | 4,413,372 → 2,521,996 |
| Shared C / runtime-native spans | — | 59,408 → 38,192 | 1,695,512 → 1,442,252 |

The 350 padding-attributed source bytes are exclusion metadata, not retained
padding instructions. Combined dispatch payload: main code 4,640,384; retained
broad gfx bin 748,016; sound 991,952; padding 0. Native spans include alignment,
page packing and outlining; shared helpers cannot be uniquely attributed to
ROM regions. Whole-file/object container cost and configure wall time cannot
honestly be divided into exact guest-region bytes/seconds. The A report retains
its finer applied-data/padding attribution and original per-region bound.

## Observed native parity gates

- Fresh isolated MAME capture: **25/25 frames**, 600–3480 every 120,
  **1,856,000 RGB pixels**, **0 mismatches / 0 maximum channel error**.
- Frame-600 main RAM: **131,072 bytes exact** against that MAME capture.
- Actual **3,600-frame** native main/sound attract WAV vs supplied reference:
  **7,270,664 bytes identical**, SHA256
  `62bac3a4d1baef4e7c2343ce987e44eb654a71f90d653b5b88c1d12d15eff05b`.
  Cycles 977,201,324; native blocks 51,507,335; **0 CPU fallback**;
  1,817,655 audio frames; frame CRC `3359f200`.
- Additional `--video compare` invariant: **250,114,560 RGB pixel checks**,
  0 differences over 3,369 supported frames. 231 explicit video-oracle fallback
  frames are presentation coverage, not CPU interpreter fallback.
- Held-out native mash **301–308 × 20,000 = 160,000 frames**: **8/8 success**,
  **2,147,127,680 native blocks**, **0 fallback**, **0 cold aborts**;
  **8/8 WAVs** and **56/56 final files** byte-identical to the preserved
  baseline held-outs. Training corpus is unchanged.
- **45/45 Python checks**, including actual compiled synthetic C/C++ behavior
  for exclusion/tier/slim boundaries, contradictory profiles, hooks, deadlines,
  flags and shared exceptions. Actual `f3rt-check` PASS. All sound callers built.

Final files per held-out seed: `palette.bin`, `graphics.bin`, `control.bin`,
`mainram.bin`, `shared.bin`, `rendered.argb`, `cpu.json`. Verified duplicate
candidate WAVs were removed only after byte comparison and SHA256 recording;
the existing golden baseline WAVs/dumps were not changed or deleted.

| Seed | Final frame CRC | Main cycles | Native blocks | Audio frames |
|---|---|---:|---:|---:|
| 301 | `4604976c` | 5,428,896,083 | 267,723,728 | 10,098,085 |
| 302 | `b7b67b19` | 5,428,896,123 | 267,347,648 | 10,098,086 |
| 303 | `e3cb0285` | 5,428,896,081 | 269,654,305 | 10,098,085 |
| 304 | `d8a94469` | 5,428,896,087 | 269,598,488 | 10,098,085 |
| 305 | `b762d87d` | 5,428,896,084 | 266,114,389 | 10,098,085 |
| 306 | `4d7e0664` | 5,428,896,084 | 269,877,341 | 10,098,085 |
| 307 | `98afe377` | 5,428,896,088 | 269,740,955 | 10,098,085 |
| 308 | `5241c010` | 5,428,896,081 | 267,070,826 | 10,098,085 |

Native block counters need not equal an untiered build's dispatch count after
page partitioning; all seven compared state/output files and audio remain exact.

## Save/load and Go checks

`go test ./...` in `netplay/server`: exit 0, package
`f3rt/netplay/server` **0.423 s** (command wall 0.938 s).
Actual `f3rt-netplay-oracle --mode snapshot --frames 6000 --seed 301
--sound-driver all`: exit 0, command wall **118.343 s**.

**60/60 exact save/load replays**: 30 native-sound + 30 oracle-sound,
points 0/1000/2000/3000/4000/5000, depths 1/7/16/31/97, memory/pixels/PCM/nonempty
sound traces exact and **0 save / 0 load allocations** for both drivers.
Each finishes 6,000 frames and `BOTH_PLAYERS_ACTIVE`; each has 30 deliberate
wall-clock perturbations. Final native/oracle state CRCs differ as expected
for different CPU state representations: `bcdefd0a` / `bc62613f`; both frame CRC
`177cbcda`, audio CRC `1f4a6e2b`, audio samples 3,029,425.
State sizes: native 4,231,509 / oracle 4,231,724 bytes.
This is the existing save/load oracle, not a new impaired-network campaign.

## GPU presentation gates

Actual Metal harness: **35/35 SUCCESS**, each scenario executed once.
The off matrix is seeds **5/6/7/41 × scales 1–4 × borders 0/48 × 4,000 frames**,
`--every 30 --layers`. Added seed-5 scale-4/border-48 linear and fit geometry
cases, each 4,000 frames, plus a 1,600-frame fit case with all six induced
boundary types at frame 1,080 and scheduled changes at
300:2 / 600:4 / 900:1 / 1200:3 / 1500:4.

Totals: **137,600 native frames**, **1,866,161,068 native blocks**,
**0 CPU fallback**, **4,669 composite / 39,747 isolated-layer checks**,
**0 mismatching pixels**. 129,515 supported frames; 8,085 explicit
presentation-oracle frames, 253 sampled fallback images and 105 actual
oracle transitions. These do not enable CPU instruction fallback.
Native audio frames summed: 69,474,824. Same-seed 4,000-frame cases preserve
native cycles/blocks/frame CRC/audio CRC/count/peak/nonzero samples.

Six induced scenarios / 18 sample groups; three native sprite-boundary branches;
21 interpolation boundary checks, 349 isolated sprite checks; **9 actual scale
changes** (5 scheduled + 4 retained-trail branch changes). Canonical snapshots,
restore/replay, native/unflagged/text/sprite and independent trail history
invariants pass. Command wall total **817.445 s**, not an isolated render
performance benchmark. Comparisons are sampled; the ending producer is induced,
not a played ending. No other-GPU or visual human judgment claim.
Exact commands/counters: ignored `build/combined-evidence/gpu/results.json`.

Actual frontend flag smoke also passes: full tiers with `--allow-fallback`
complete 600 native frames with zero fallback; slim with that flag exits 1
before starting emulation, retaining its strict-only policy.

## Foreground human run

Only after `binsize-combined-2-gated`, launched this actual combined executable
without a frame/time limit:

```sh
./build/landmakr --video-backend gpu --video-scale auto-integer --video-border 48
```

Cwd: `wt/binsize-combined`; pid 20834. **423.092 s (7m03s)**,
**24,886 frames**, normal exit **0**. No collector early-stop, automated game
inputs, forced quit or synthetic Escape. Initial windowed Cocoa/GPU presentation:
internal **1664×928**, pixels **2496×1392**, scale **4**, nearest, interpolation
off. Later window pixels **3024×1686**, scale stayed 4.

The first AppKit activation capture still showed the terminal. System Events
then foregrounded the game; `human-combined-focused.png` was inspected and shows
the real game surface. This is not the profiling experiment's unseen unattended
window. Run counters: **334,119,851 native blocks**, **0 CPU fallback**,
cycles **6,755,175,389**, **12,565,048 audio frames**, frame CRC `7a829644`,
**0 abort/crash logs**. Pacing: 2 clock resyncs, 0 audio queue drops, queue
mean/max **30.1716 / 55.0721 ms**.

Normal frontend quit is observed; Escape versus window-close and actual match
count are not independently logged. No human-played ending or completed-match
count is claimed. The noninstrumented run does not change the frozen profile.
Command/start/end/duration, output and visibility proof remain ignored under
`build/combined-evidence/human-{combined.json,combined.log,visibility.json,gate.json}`
and `human-combined-focused.png`. Local checkpoint: `binsize-combined-3-human`.

## Evidence and limits

Ignored `build/combined-evidence/` holds commands, durations, exit codes, logs,
`profile-audit.json`, `sizes.json`, regional reports, effective tier flags,
MAME captures, attract-gate JSON, eight per-seed gate JSONs/hashes, runtime
exclusion/cold-hit probe logs, `gpu/results.json` and `netplay/results.json`.

Only Japanese Land Maker and this Darwin arm64/Metal environment are validated.
Finite profiles and seeded gates are not all-state proof of every exclusion,
ending or indirect path. Full coverage means every existing nonexcluded entry
is retained, not that every baseline unsupported lowering is implemented.
Slim retains the same hot union and still has its historical 7/8 held-out abort
risk; no generally playable slim claim. No original campaign tail was recovered,
no other ROM set or GPU-host correctness claim, no new network-impairment claim.
