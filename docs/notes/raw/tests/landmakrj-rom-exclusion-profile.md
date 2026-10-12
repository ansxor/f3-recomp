---
source_url: docs/developer/BINSIZE-EXCLUDE.md
ingested: 2026-10-11
sha256: 2d8ab76bbe9b801a7c6f1d6619eff378c355f9b1f24e55f49f02ce2d329d94b4
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository record of runs (secondhand: command output and artifacts under ignored build/ were not retained); source doc deleted after ingest; tools/scan_rom_exclusions.py, tools/profile_rom_access.py"
---
<!-- excerpt: docs/developer/BINSIZE-EXCLUDE.md lines 45-48 -->
The main tail ends two bytes before ROM end: the final checksum word is not FF.
Actual sound samples and tile/sprite pixel ROMs live in separate runtime regions
and were never inputs to discovery. They offer **zero** generated-code savings;
only tables/padding inside the two CPU program images are candidates.

<!-- excerpt: docs/developer/BINSIZE-EXCLUDE.md lines 72-92 (scanner evidence hierarchy) -->
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

<!-- excerpt: docs/developer/BINSIZE-EXCLUDE.md lines 120-174 (applied sets, counterexample, native profile) -->
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

<!-- excerpt: docs/developer/BINSIZE-EXCLUDE.md lines 226-241 (gate results) -->
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

<!-- excerpt: docs/developer/BINSIZE-EXCLUDE.md lines 273-281 (other sets) -->
## Other sets

Only `landmakrj` is applied and gameplay-validated. The repository also has a
World `landmakr` config, but its scanner run failed on the absent `e61-19.20`
program lane in the supplied Japan ROM directory. ROM directories for `gunlock`,
`puchicar`, `recalh` and `tcobra2` are present; their game configs/runtime ports
are not. Porting them is not a cheap exclusion experiment. No exclusion or
correctness claim is made for these sets.
