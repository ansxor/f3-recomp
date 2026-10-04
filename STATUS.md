DONE: threaded CPU, exact SDL3 GPU compositor and guarded opt-in linear/fitted PF2 water interpolation. Metal verified; ending coverage is induced, not a played ending.

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
