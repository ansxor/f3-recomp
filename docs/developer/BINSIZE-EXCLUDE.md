# Binary-size exclusion experiment

This records the historical exclusion-only experiment at
`binsize-exclude-3-applied`. The combined checkout now also enables full-coverage
compile tiers by default; use `F3_PROFILE_DEFAULT_TIERS=OFF` and clear any
explicit `F3_PROFILE_TIERS` cache override to measure exclusion-only code. The default tiers need the local, gitignored `profiles/landmakrj.profile` (regenerate it per [BINSIZE-PROFILE.md](BINSIZE-PROFILE.md)); without it, configure fails unless `F3_PROFILE_DEFAULT_TIERS=OFF`.
[BINSIZE-COMBINED.md](BINSIZE-COMBINED.md) records the semantic ABI 3 merge,
unchanged six intervals, profile contradiction guards and combined gates.


## Measurement checkpoint (before implementation)

Land Maker Japan, Release Clang/arm64, `all_aligned` main and native sound.
Timed fresh configure: **14.864 s**; build (`landmakr` and gameplay harness,
`-j 6`): **54.537 s**. Worktree binary: **84,436,984 bytes**;
Mach-O `__TEXT`: **56,901,632 bytes**. The supplied reference binary is
84,291,656 bytes / 56,770,560-byte `__TEXT`; it predates this worktree's GPU
frontend integration, so this experiment uses its own fresh baseline.

| Provisional ROM region | Native functions | Registered PCs | Function C bytes | Dispatch C bytes | Object `__text` bytes | Dispatch object bytes |
|---|---:|---:|---:|---:|---:|---:|
| Main code and other data | 10,878 | 336,775 | 126,308,600 | 13,274,066 | 25,937,684 | 5,388,400 |
| Main graphics-looking hypothesis, `0x020000..0x088000` | 6,656 | 209,535 | 92,068,080 | 8,206,148 | 18,381,452 | 3,352,560 |
| Main FF padding, `0x007030..0x010000`, `0x11b362..0x1ffffe` | 0 | 486,966 | 0 | 20,452,572 | 0 | 7,791,456 |
| Sound program and other data | 93,326 | 114,501 | 40,359,960 | 4,958,487 | 9,616,948 | 1,832,016 |
| Sound FF padding, `0xc1e45a..0xc20000`, `0xc39a30..0xc80000` | 0 | 147,643 | 0 | 6,053,363 | 0 | 2,362,288 |

C columns attribute complete native functions and dispatch rows by their guest
start address; declarations and file preambles are separate overhead. Object
columns measure symbol extents in Mach-O object `__text` and 16-byte dispatch
records, not relocation/debug/container bytes. All generated C including
preambles/declarations totals **316,308,113 bytes**. Full generated object file
sizes are preserved in the local measurement JSON.

**Upper bound before implementation:** these padding ranges contain no native
functions already; they account for **10,153,744 dispatch bytes**, not 10 MB of
native instructions. Removing padding cannot materially reduce native `__TEXT`.
The coarse graphics-looking hypothesis contributes another **21,734,012 bytes**
of native text and dispatch records. Thus the candidate-associated text/table
payload is **31,887,756 bytes**, plus pooled linker metadata that is not
region-attributable. This is an accounting ceiling on these payloads, not a
promised file-size delta: the entire graphics-looking interval is not proven
data and must not be applied wholesale on this measurement alone.

The main tail ends two bytes before ROM end: the final checksum word is not FF.
Actual sound samples and tile/sprite pixel ROMs live in separate runtime regions
and were never inputs to discovery. They offer **zero** generated-code savings;
only tables/padding inside the two CPU program images are candidates.

Commands and raw evidence remain ignored under `build/exclude-evidence/`:
`baseline-timing.json`, `baseline-initial-regions-worktree.json`, and
`baseline-accounting-bound.json`. No ROM bytes, generated C or binaries belong
in a commit.

## Scanner and native access evidence

```sh
PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages \
  python3 tools/scan_rom_exclusions.py --config games/landmakrj/config.toml \
  --rom-dir /path/to/roms/landmakr --cpu main \
  --output build/exclude-main.json --toml build/exclude-main.toml
# Repeat with --cpu sound.

# Requires a completed build with CMAKE_EXPORT_COMPILE_COMMANDS=ON.
PYTHONPATH=/private/tmp/sb-context-oracle/lib/python3.13/site-packages \
  python3 tools/profile_rom_access.py --build-dir build \
  --output-dir build/rom-access --rom-dir /path/to/roms/landmakr \
  --seeds 1 2 3 4 5 6 7 8 --frames 20000 --jobs 6
# Add --profile build/rom-access/profile.json to the scanner.
```

Scanner evidence hierarchy:

1. Observed native instruction-fetch spans and configured/vector/rooted code
   references block proposals, including undecodable explicit entry points.
2. Typed game-data intervals (including actual tile-descriptor bus reads by
   documented HLE helpers) are considered independently of entropy.
3. Exact long 00/FF fills and exact mixed 00/FF periodic fills are structural
   candidates; alignment trims inward and preserves nonmatching trailers.
4. High-entropy windows are statistical candidates, not code/data proof.
   Ordinary reads alone cannot prove their unread portions are data.

JSON retains rejected/conflicting candidates and exact read/fetch intersections.
The TOML output is an unapplied, nonoverlapping proposal list. No scanner modifies
a config. Profiles do not prove anything about unplayed states or absent reads.

The recorder builds private copies of native generated code, marking every
executed instruction label (including shared exception entries), and copies of
the two runtime bus readers. Main accesses are recorded byte-by-byte, including
inline-helper reads; sound accesses retain their actual bus width. It uses the
existing seeded gameplay harness and native sound. Normal generated/runtime
files are untouched; profiling calls do not enter production code.

## Exclusion contract

Top-level `[[exclude]]`: `cpu` (`main` default, or `sound`), even guest `start`,
exclusive even `end`, nonempty `reason`/`evidence`. Intervals must be within the
CPU program image, sorted by the parser and nonoverlapping. They exclude
instruction starts, not memory reads or extension words of retained starts.

Vectors, explicit entries, hooks and explicit main jump-table targets in an
exclusion reject generation. Exhaustive decoding of data invents direct
branches, so apparent direct-transfer conflicts are reported for runtime
enforcement rather than misclassified as proven control flow. Unknown computed
or script/pointer-table destinations receive the same runtime guard.

ABI 3 registers immutable main exclusion spans, disjoint from native entries.
Missing/odd excluded main PCs and 24-bit bus aliases fail before fallback even with interpretation
enabled. Compact sound tables cover exactly the every-even exclusion complement;
dispatch subtracts preceding excluded words, with no second allocation/map.
An excluded sound target throws before an opcode read. Timing, `f3_cpu` layout
and canonical machine snapshots are unchanged.

The synthetic discovery/generator/scanner suite and actual the runtime tests cover
bounds, overlap, entry/table conflicts, lazy flags/deadlines and fallback
precedence. A real-ROM smoke additionally forces even/odd starts and both ends
of all six applied ranges, plus `JMP (A0)` into an excluded range: **25** explicit
address/range failures, zero fallback, zero sound instruction fetches.

## Applied sets and first breaking candidate

All intervals below are half-open. The main checksum word at `0x1ffffe` remains
included. Exclusions change dispatch/discovery, never the ROM data image.

| CPU | Applied interval | Evidence |
|---|---|---|
| Main | `0x007030..0x010000` | Exact FF fill |
| Main | `0x11b362..0x1ffffe` | Exact FF fill; final non-FF checksum word retained |
| Main | `0x020000..0x088000` | Graphics/script-table bank; no rooted code/reference; native read/fetch profile and parity gates |
| Sound | `0xc1e45a..0xc20000` | Exact FF fill |
| Sound | `0xc39a30..0xc80000` | Exact FF fill, including unpopulated chip halves |
| Sound | `0xc20000..0xc39a30` | Sequence/header bank documented in `SOUND-DRIVER.md`; no rooted code/reference; native read/fetch profile and WAV gates |

Progression:

1. **A: four FF fills.** All gates passed. No native functions removed;
   savings are primarily dispatch records, not native code.
2. **B: A plus the graphics/script and sound sequence banks.** All gates passed;
   this is the selected config.
3. **C: B plus statistical-only main `0x002000..0x003000`. First tested breaking
   set.** Entropy **7.689694 bits/byte**, 252 distinct bytes, 79/128 sampled
   aligned starts decodable. Generation succeeded, but the real strict-native
   attract command exited **1** after **1.024 s**, before frame-600 capture:
   `Excluded main CPU instruction at PC 0x2f84 in [0x2000, 0x3000)`.
   This deliberately overrides the scanner's rooted-code/fetch rejection,
   demonstrating why entropy alone is unsafe. The range is **not applied**.

This is a tested conservative set, not a maximal safe boundary or an all-state
proof. Unplayed/cold indirect paths remain a risk; any excluded destination is
loud rather than silently interpreted. The scanner preserves its rejected
counterexample and does not propose it in TOML.

### Native profile

The recorder used **A**, retaining all data-bank native entries, for seeds
**1–8 × 20,000 frames = 160,000 frames**, native main and sound, zero fallback.
Every diagnostic WAV was byte-identical to its uninstrumented baseline.

| CPU | Unique fetched bytes, whole image | Unique read bytes, whole image | Fetched bytes inside B's added bank | Read bytes inside B's added bank |
|---|---:|---:|---:|---:|
| Main | 103,856 | 213,826 | 0 | 147,494 / 425,984 |
| Sound | 23,984 | 62,488 | 0 | 31,834 / 105,008 |

Main typed descriptor reads cover **68,904 bytes** inside the graphics/script
bank. Another 12 typed/read bytes overlap helper code and are blocked by the
scanner, not excluded. The whole-bank decisions also use ROM layout and rooted
references; observed reads do **not** prove the unread portions are data.
Rooted discovery still reports **22 main / 4 sound** unresolved transfers.
Known code, configured hooks and observed fetches take precedence over entropy.

The recorder supports the verified **landmakrj** runtime/build only. A
different game identity is rejected, not silently recorded with Japan helper
addresses. A reused-output smoke included an obsolete duplicate registration
object; only current compile-command objects were linked.

### Size and count checkpoints

Release AppleClang 21 / arm64; same GPU-enabled frontend, native main and sound.
Counts are dispatch entries, not exclusion-metadata initializer rows. Generated
source/object inventories use active `source_files` / compile commands; stale
unreferenced shards from earlier generations are not counted.

| Metric | Baseline | A: fills | B: selected |
|---|---:|---:|---:|
| `landmakr` file bytes | 84,436,984 | 74,201,160 | **42,778,072** |
| Mach-O `__TEXT` segment bytes | 56,901,632 | 56,901,632 | **32,604,160** |
| Generated C bytes | 316,308,113 | 289,803,153 | **164,220,955** |
| Full generated object file bytes | 105,939,312 | 90,709,832 | **52,023,360** |
| Main registered PCs | 1,033,276 | 546,310 | 336,775 |
| Main native functions | 17,534 | 17,534 | 10,878 |
| Main decoded instruction starts | 464,523 | 464,523 | 271,762 |
| Main native instruction starts | 460,668 | 460,668 | 268,911 |
| Main excluded aligned candidates | 0 | 486,966 | 699,958 |
| Sound registered PCs | 262,144 | 114,501 | 61,997 |
| Sound emitted functions, including 3 shared exceptions | 93,329 | 93,329 | 49,423 |
| Sound excluded aligned candidates | 0 | 147,643 | 200,147 |

B saves **41,658,912 file bytes (49.3%)**, **24,297,472 `__TEXT` bytes (42.7%)**,
and **152,087,158 generated C bytes (48.1%)**. Main generated C is 140,072,975
bytes; sound is 24,147,980. Remaining unsupported lowering candidates are
**2,851 main / 694 sound**; none executes during the validated gates.

| Selected B region | Native functions | Registered PCs | Function C bytes | Dispatch C bytes | Object native `__text` bytes | Dispatch object bytes |
|---|---:|---:|---:|---:|---:|---:|
| Main code and other data | 10,878 | 336,775 | 126,308,600 | 13,274,066 | 25,937,716 | 5,388,400 |
| Main FF padding | 0 | 0 | 0 | 0 | 0 | 0 |
| Main graphics/script bank | 0 | 0 | 0 | 0 | 0 | 0 |
| Sound program and retained tables | 49,420 | 61,997 | 19,432,690 | 2,681,785 | 4,551,664 | 991,952 |
| Sound FF padding | 0 | 0 | 0 | 0 | 0 | 0 |
| Sound sequence bank | 0 | 0 | 0 | 0 | 0 | 0 |

Shared exceptions, helper text and exclusion metadata are overhead, not native
functions/dispatch entries in removed regions. B adds a sound-bank candidate
after the initial padding/graphics bound; it removes another **5,905,348 bytes**
of native text/table payload from the initially measured sound-program/data
row. Linker constants, unwind information and link-edit data also shrink;
the initial **31,887,756-byte** payload bound was not a promised file delta.

Fresh comparable configure/build times (`landmakr` + gameplay harness, `-j 6`):
**14.864 / 54.537 s baseline**, **7.844 / 26.999 s final-source B**. No experiment
gate/profile processes ran during those timed builds; unrelated host workload
is not controlled. A's initial **71.517 / 104.686 s** build included additional
caller targets and overlapped baseline gates. B's initial **18.947 / 82.937 s**
build overlapped profiling/gates. Those contended times are not speedup evidence.

### Gate results

For **each successful set A and B**:

- **25/25** MAME attract frames, frames 600–3480 every 120:
  **1,856,000 RGB pixels**, zero mismatches, maximum channel error **0**.
- Frame-600 main RAM byte-identical to MAME.
- 3600-frame native attract WAV byte-identical to the supplied baseline;
  frame CRC **3359f200**, **51,507,335** native blocks, **0** fallback,
  **1,817,655** audio frames. The 231 boot video-oracle frames are video behavior,
  not interpreter fallback.
- Seeds **1–8 × 20,000 = 160,000** headless/unthrottled native frames:
  all eight WAVs byte-identical to the same-seed baseline, all six final
  RAM/graphics/palette/control/shared/pixel dumps identical, **0** fallback.
  **2,140,582,066** native main blocks and **80,784,686** stereo audio frames
  per eight-seed set; cycles/PCs/CRCs match the baseline.

| Seed | Final CRC | Main cycles | Native blocks | Sound PC | Audio frames |
|---|---|---:|---:|---|---:|
| 1 | `7f1532e8` | 5,428,896,121 | 269,834,801 | `c10b0e` | 10,098,086 |
| 2 | `5dbf9d43` | 5,428,896,122 | 265,832,959 | `c18698` | 10,098,086 |
| 3 | `78856d11` | 5,428,896,111 | 267,947,180 | `c1845a` | 10,098,086 |
| 4 | `f1100489` | 5,428,896,085 | 269,096,433 | `c10b10` | 10,098,085 |
| 5 | `ea5099f7` | 5,428,896,087 | 267,206,500 | `c18c54` | 10,098,085 |
| 6 | `d458cb8d` | 5,428,896,108 | 261,561,677 | `c18f60` | 10,098,086 |
| 7 | `95dc1a53` | 5,428,896,122 | 269,423,624 | `c19818` | 10,098,086 |
| 8 | `b452bfae` | 5,428,896,119 | 269,678,892 | `c18fce` | 10,098,086 |

An initial B attempt was interrupted by disk exhaustion: partial seed-3/4 WAVs
and absent final dumps were not counted as passing gates. Owned, obsolete
diagnostic artifacts were removed; interrupted/unstarted seeds 3–8 then passed.
Successful seed-1/2 results were retained. After byte comparisons, long duplicate
WAVs were removed; their SHA-256 manifests and state/counter evidence remain.

The real-ROM alias probe found and fixed a cold rejection-path bug:
`PC 0xff020000` maps to excluded physical `0x020000`. Before normalization it
counted one diagnostic fallback; afterward it throws the explicit address/range
error with **zero** fallback. The permanent runtime check covers even/odd aliases
as well as half-open boundaries. This does not change the native zero-fallback
gameplay path.

Local evidence: `baseline-*`, `padding-*`, `data-*`, `final-clean-*`,
`padding-profile.json`, `main-profile-scan.json`, `sound-profile-scan.json`,
`seeded-audio-manifest.json`, `profile-audio-parity.json`, and
`break-entropy-runtime-result.json` under ignored `build/exclude-evidence/`.
Generated diagnostic copies and throwaway sources are not shipped.

## Other sets

Only `landmakrj` is applied and gameplay-validated. The repository also has a
World `landmakr` config, but its scanner run failed on the absent `e61-19.20`
program lane in the supplied Japan ROM directory. ROM directories for `gunlock`,
`puchicar`, `recalh` and `tcobra2` are present; their game configs/runtime ports
are not. Porting them is not a cheap exclusion experiment. No exclusion or
correctness claim is made for these sets.

