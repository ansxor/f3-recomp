DONE: landmakrj ROM exclusions cut binary size 49.3%; zero-fallback gates pass. Finite coverage, not all-state proof; other sets unvalidated.

# Binary-size experiment — ROM exclusions

## Local checkpoints

- `binsize-exclude-1-measure`: per-region baseline measured before implementation;
  padding/graphics text-table payload bound 31,887,756 bytes, not a promised file delta.
- `binsize-exclude-2-scanner`: proof-first proposals plus native instruction-fetch/data-read
  recording; ABI 3 exclusion enforcement is included in the preceding infrastructure commits.
- `binsize-exclude-3-applied`: six explicit main/sound intervals in
  `games/landmakrj/config.toml`; no ROMs, binaries or generated code committed, no pushes.

## Measured result

Release AppleClang 21 / arm64, GPU-enabled frontend:

| Metric | Before | Selected exclusions |
|---|---:|---:|
| `landmakr` file bytes | 84,436,984 | 42,778,072 |
| `__TEXT` segment bytes | 56,901,632 | 32,604,160 |
| Generated C bytes | 316,308,113 | 164,220,955 |
| Generated object file bytes | 105,939,312 | 52,023,360 |
| Main registered PCs / native functions | 1,033,276 / 17,534 | 336,775 / 10,878 |
| Sound registered PCs / emitted functions | 262,144 / 93,329 | 61,997 / 49,423 |
| Fresh configure / two-target `-j 6` build | 14.864 / 54.537 s | 7.844 / 26.999 s |

## Verified behavior

- Both successful candidate sets: **25/25** MAME attract frames,
  **1,856,000 RGB pixels**, zero differences; exact frame-600 main RAM and
  baseline-identical 3600-frame attract WAV.
- Each set: seeds **1–8 × 20,000 = 160,000** native main/sound,
  headless/unthrottled frames; all eight seeded WAVs and six final memory/pixel
  dumps identical to baseline; **2,140,582,066 native blocks**, **0 fallback**.
- Untrimmed-data profile: another **160,000** native frames, all eight WAVs
  identical; **0 instruction-fetch bytes** inside the two added data banks.
  Partial reads and rooted references support the decisions, not universal proof.
- First breaking set: add statistical-only main `0x2000..0x3000`.
  Actual attract run exits **1** at excluded **PC `0x2f84`**, before frame 600.
  The scanner rejects this code-containing entropy window; it is not applied.
- **25** forced even/odd/range-end/computed-target failures with zero
  fallback/sound fetch. A real `0xff020000` physical-alias probe fails before
  interpretation; its previously observed one-instruction fallback is fixed.
- **26** Python discovery/lowering/scanner checks pass; actual `f3rt-check`
  passes, and every migrated sound caller compiles. Final-source production
  repeats all MAME/RAM/attract-WAV gates. The reused-output recorder completes
  3600 seeded native frames with a byte-identical production WAV.

## Limits and evidence

Only `landmakrj` is applied/validated. World `landmakr` lacks required supplied
program lanes; `gunlock`, `puchicar`, `recalh` and `tcobra2` lack game configs/
runtime ports here. No other-set correctness claim. This is not a maximal safe
boundary or proof of unplayed endings/cold indirect paths. Excluded targets,
including odd PCs and 24-bit bus aliases, fail loudly rather than invoking
interpreter fallback. ROM reads and canonical snapshots remain unchanged.

Per-region accounting, candidate ladder, seed counters, command recipes,
contention/disk-interruption notes and evidence paths:
[docs/BINSIZE-EXCLUDE.md](docs/BINSIZE-EXCLUDE.md).

# Phase 5 — GPU video

## Distinct local checkpoints

- `gpuvideo-1-threaded-cpu` — `e7dc01c`: persistent CPU row workers, exact serial reference and measured improvement.
- `gpuvideo-2-gpu-parity` — `93e9774`: portable SDL3 GPU/Metal compositor, no interpolation, exact oracle presentation.
- `gpuvideo-3-interp` — separate opt-in `--video-interp off|linear|fit`; CPU and off remain defaults.

No pushes, ROMs or binaries committed. Captures, CSVs and logs remain outside
this worktree under `/tmp/f3-gpuvideo`: `threaded-cpu`, `gpu-parity`,
`line-profile`, `interpolation`, and `interpolation-smoke`.

## Verification

- Off parity matrix rerun after interpolation integration: seeds 5/6/7/41 × scales 1–4 × borders 0/48 × 4000 = 128,000 native frames; 4320 composites and 4096 comparisons per each of nine isolated layers, zero mismatches.
- Linear/fit: 22 more 4000-frame runs, 88,000 native frames, 2970 sampled images, 768 accepted sprite-isolation checks and 20×11 invalid-input/boundary fixtures. Outside/whole boundary rows, declined/oracle images, canonical bytes and sprite contributions remain exact. Same-option off/linear/fit native/audio/state CRCs, cycles and native block counts match; zero instruction fallback.
- Native output retains the existing 25/25 MAME comparison and frame-600 RAM. Fresh headless fit flags retain 250,114,560 native RGB checks with zero differences and byte-identical integration WAV. Native frame CRC `3359f200`, 51,507,335 native blocks, zero CPU fallback.
- Independent ordinary CPU-backend snapshots/presentation and allocation-free save/restore are verified; four-frame expanded trail retention regression remains exact.
- `F3RT_GPU=OFF` frontend builds/runs, runtime device check passes, and an actual CPU window is exercised. Actual 4x Metal off and fitted-water windows/internal surfaces are inspected.

## Performance and visible gain

Scale-4/border-48 parity checkpoint native+fenced-GPU render budget:
mean/p95/worst **10.443/11.082/11.720 ms**. Actual off frontend:
3600 frames in **31.56 s / 114.1 fps**.

Accepted water frame 1560, 100 warmed frozen samples, scale 4/border 48:
GPU linear **4.640/6.201/7.043 ms**; fit **4.848/6.963/7.595 ms**
(mean/p95/worst, includes uploads/fence/readback). Actual fitted frontend:
3600 frames in **30.31 s / 118.8 fps**, native CPU/audio still running.
CPU/GPU scale 1/2/4 tables and precise timing scopes: `docs/GPU-VIDEO.md`.

Character-select water is PF2. Geometry uses validated screen rows 152–255;
measured continuous palette prefix is 152–237. Linear visibly removes 4x
water stair steps; fitted sampling additionally smooths source packing/palette
grading. Three-mode frames/details at 1409/1500/1560 and PF0 sine frame 1300
are captured. No outside-run differences or smeared horizon band.

## Qualified limits

- Only the complete known normal PF2 water/puzzle-board profile interpolates. No raw per-field enable/active-descriptor API was claimed; known program origin plus full normalized-row validity/shape/continuity checks establish the range. Unknown effects, including PF0 sine, remain unchanged.
- RGB bank 0↔1 has a real discontinuity and remains discrete; palette indices/pen roles and sprites are never interpolated. Fitted sampling is a guarded approximation, not the ROM palette table's proven analytic function.
- Bitmap, trails, flip, unknown writer and ending-producer fallback/recovery are additionally induced. A campaign ending was not played through; unsupported geometry remains exact oracle output, not reconstructed HLE.
- Runtime evidence is M5/Metal. SPIR-V/MSL compile/resource contracts are verified; other GPU hosts are not runtime-tested. GPU availability errors are explicit; CPU backend remains available.
- Headless/native captures, CRCs, WAV and canonical snapshots remain CPU-produced. Netplay remains scale 1/border 0; interpolation is inactive at scale 1.

Design, boundary-candidate comparison, per-scene acceptance/rejection counts,
commands, captures and full performance evidence: [docs/GPU-VIDEO.md](docs/GPU-VIDEO.md).
