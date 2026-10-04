BLOCKED: combined build/basic native checks pass; full automated gates and human Escape run pending. Finite-profile/exclusion coverage, landmakrj only; slim remains rejected for general play.

# Combined binary-size experiment

ABI 3 semantic merge complete. Default full-coverage hot `-O2` / cold `-Oz`
retains 336,775 main and 61,997 sound entries; exclusions are filtered before
tier assignment. Frozen seeded/campaign corpus re-merges byte-identically,
with zero excluded profile records. All 45 Python checks and real `f3rt-check`
pass. Real 600-frame native run: zero fallback. Runtime probes pass 48 main
and 12 sound excluded-PC/odd/alias/trace checks, three half-open boundaries
and both CPUs' retained cold NOPs. Full gates/human run remain pending.

## Historical integration and experiment evidence

DONE — Phase 8 sprite checkpoint `gpuvideo-8-sprites`, following `gpuvideo-7-general-lineram`. Existing CPU/GPU zoom already samples ROM texels on the N-grid; no missing supported transform field or justified additional sprite resampler. Exact off/native behavior retained. Metal exercised; other GPUs/another monitor and a played campaign ending unverified.

## Delivered

- General per-playfield/per-field linear and anchored shape-preserving fit from Phase 7 remain intact: PF0 sampled wave and both valid PF2 zoom halves. Same-scale native subrow-zero and unflagged samples stay exact. `--video-interp-fields none|geometry|palette|geometry,palette` defaults geometry only; palette bank blending is separately opt-in. Alpha/clip/mosaic/priority/column jumps stay native/discrete.
- Sprite X/Y scale and flips already reach the internal grid before fixed-point rounding. Documented the actual equations, unchanged constant128/255 phases, nominal pre-round culling, minimum-one Y splats and first-opaque precedence. ROM-submitted origins contain no fractional native placement or rotation matrix. No fake new sprite mode, rescaled raster phase, filtered artwork or new-color gain claim.
- Requested diagnostics now capture all nine isolated layers. Added actual-native-producer `--inject-sprite-boundaries`: crushed opaque overlap, mirrored sampled zoom and nominal top-edge culling, with visible ROM witnesses and branch restore/replay.
- README/site/ABI/GPU design/NOTES Video results updated. Canonical scene/machine/snapshot layouts, native CPU/sound, capture/CRC/WAV and netplay remain unchanged. No pushes, ROMs, generated assets, captures or throwaway helpers committed.

## Exercised Phase 8 proof

- Frozen actual seed5 frames1080/3404/6000: scales1–4/borders0,48;240 complete/all-nine CPU-GPU comparisons,120 public CPU-reference cross-checks,96 original-descriptor category checks and384 linear/fit geometry/both isolated sprite checks,all exact. Canonical scene/machine bytes,state CRC/frozen native replay exact;zero fallback instructions.
- Existing4x versus nearest1x: frame1080 SP2/SP3 changed2560/8832 pixels;3404 SP3 changed2149;6000 every SP group changed0. Global RGB/source-index counts unchanged. Unit-size descriptor subsets gain0. Native lattice differs105/368/93 across scales from existing CPU phases; same-scale off-versus-line-mode sprite/native-row differences are0. This is existing sampling coverage, not a new Phase8 sprite enhancement or new artwork.
-24 scale/border/off-linear-fit runs =25,920 native frames/72 native boundary branches,zero layer/composite mismatches. Crushed overlap visible17/34/51/68 pixels at1/2/3/4x; mirrored zoom53/174/367/671;nominal top cull0. Mode changes preserve native/audio CRC,cycles/blocks and same-geometry state CRC. Extra1600-frame fit/both run exercises9 runtime scale changes,retained trail snapshots and all six induced scenario types,exact restore/recovery;ending is induced,not played.
- Isolated100-repeat border48/frame1080 mean/p95/worst ms:4x CPU7.510/8.818/11.485,GPU off2.063/2.429/4.695,fit geometry2.088/2.186/2.274. Frame6000 CPU7.394/8.870/10.093,GPU off1.981/2.188/2.267,fit geometry2.038/2.098/2.191. Full1/2/4x plus linear/palette tables are in docs/external logs;GPU includes fence/readback,not kernel or whole-frontend time. No extra sprite pass/work or new optimization claimed.
- Actual foreground Cocoa/Metal off/linear/fit geometry key-event windows at4x/border48:each1600 frames,101,634,560 exact native RGB comparisons,matching native CRC90d70624/cycles434311748/native blocks23892754/audio frames807846,zero fallback instructions and byte-identical WAVs. Actual introduction/character-select and isolated sprite surfaces inspected. Each has3 screenshot-induced pacing resyncs;off/linear0 queue drops,fit1/66.328ms peak. Captures are not claimed free. Non-activated background screenshots are not frame-aligned proof.
- Integrated build,`f3rt-check` and harness help pass. GPU-off Cocoa frontend accepts field controls and presents native boot output. Source-helper programs/binaries removed after smoke;raw survey/results CSVs losslessly compressed to .csv.gz to clear owned disk pressure.

## Retained Phase 7 acceptance

-56 cases /224,000 native frames:3112 off CPU-GPU composites and2544 in each isolated layer,2248 native/unflagged/text/sprite checks,252 visible-ROM field-boundary guards,136 runtime scale changes;all exact. Same-seed native/audio/state signatures unchanged.
-4x geometry-only captures add0 non-palette RGB;palette+geometry water adds339 colors in18,745/18,746 pixels,nearest-native-entry mean4.543/3.407/max6.928 RGB units. Palette remains off by default. Static floor6000/attract-alpha2400/results4850 gain0 for measured reasons.
- Prior paced automatic-integer fit geometry at2496x1392/internal1664x928 audio mean27.984/max37.499ms,zero drops;fresh3600 native headless RGB/WAV invariants and GPU-off build/device proof. Existing MAME25/25 acceptance path unchanged;no new MAME capture comparison claimed.

## Evidence and limits

- Detailed equations,field checklist,measurements and commands:docs/GPU-VIDEO.md. External evidence:/tmp/f3-gpuvideo/general and /tmp/f3-gpuvideo/sprites/{proof*,boundaries,transitions.*,bench*,windows}.
- Static floor is PF0 pre-drawn perspective,not PF2 water. Actual line alpha/clip/priority/column controls are discrete blocks;no smooth Y-zoom or mosaic ramp observed. Global flip,bitmap,retained trails,unknown writers and unsupported ending stay correctly presented native oracle fallbacks.
- CPU and interpolation-off remain defaults;netplay still fixed scale1/border0. No other GPU/second-monitor or played campaign ending claim.

DONE — `gpuvideo-8-sprites`: supported sprite sampling proven complete, all reachable requirements exercised; limits above.


---

## Merged experiment A: ROM exclusions (from binsize-exclude)

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

# Binary-size experiment B — execution profiles

Complete report: [docs/BINSIZE-PROFILE.md](docs/BINSIZE-PROFILE.md).

Strict native main + sound. Zero fallback in all successful candidate runs and all held-out abort diagnostics. No pushes, ROM bytes, generated code or binaries committed.

## Result

| Build | File bytes | __TEXT bytes | Generated C bytes | Main / sound retained entries |
|---|---:|---:|---:|---:|
| Supplied reference | 84,291,656 | 56,770,560 | 316,308,113 | 1,033,276 / 262,144 |
| Fresh ordinary full | 84,458,008 | 56,918,016 | 337,607,288 | 1,033,276 / 262,144 |
| Instrumented full | 105,856,488 | 78,102,528 | 337,607,288 | 1,033,276 / 262,144 |
| A: full hot -O2 / cold -Oz | 62,699,592 | 31,473,664 | 336,532,136 | 1,033,276 / 262,144 |
| B: explicit slim | 6,003,528 | 4,571,136 | 19,119,297 | 30,146 / 7,519 |

A reduces reference file / __TEXT by 25.616% / 44.560% while retaining every baseline entry. Hot/cold main pages, sound shards and shared exception helpers are separately compiled. Real cold NOPs and -Oz shared exception handlers executed without fallback.

B removes cold code rather than registering cold stubs. Both CPUs' missing entries abort with CRC/address/re-profile hint and durable recording; real probes verified no opcode progress or interpreter execution, even with main fallback deliberately enabled. Held-out aborts demonstrate that this corpus cannot support a generally playable slim cutover.

## Gates

- Fresh reference MAME captures: frames 600–3480, step 120; 25/25 comparisons / 1,856,000 pixels each, zero RGB differences for reference, fresh full, instrumented, tiers and slim.
- Frame-600 main RAM exact for every build; attract WAV byte-identical to native reference.
- Seeds 101–108 × 20,000 frames: full, instrumentation, A and B each 8/8 success; zero fallback; all eight WAVs and seven final state files byte-identical to reference.
- Held-outs 301–308 excluded from profiling: reference 8/8 completed; B 1/8 completed (seed 306, WAV/state exact), 7/8 loud cold aborts. Exact addresses/frames in the report.
- 22 unique passing parser/discovery/compiled C/C++ regressions. Actual `f3rt-check` PASS. Final corrected instrumentation frontend also exercised --profile-out and all attract parity gates.
- Sequential merge count doubling, active-writer exclusion and independent 35.051-second SIGKILL periodic-snapshot preservation verified.

Final configure/build seconds (-j 6, reused runtime objects): full 14.913 / 47.911; instrumentation 22.798 / 98.180; A 14.851 / 48.650; B 6.587 / 3.948. Initial clean-build times, regional source/text/table attribution and observed regional compiler-duration sums are in the report. Timings include concurrent host load, not isolated performance claims.

## Frozen profile and real human provenance

`profiles/landmakrj.profile`: 1,186,177 bytes, CRC keys main `15a59a08`, sound `5a7e9117`; addresses/counts only. SHA256 `5ae3c7b6e01b07a569ab984a7409cce33ab13282090435188ad65cb61ccffebb`.

| Descriptive region | Ever executed / original entries |
|---|---:|
| Main code bin | 28,817 / 290,024 |
| Main homogeneous padding | 0 / 486,966 |
| Main gfx-looking bin | 1,329 / 256,286 |
| Sound program | 7,519 / 262,144 |

Main total 30,146 / 1,033,276 (2.9175%); sound 2.8683%. Gfx-looking contains actual executed entries: entropy is not a discovery exclusion proof.

The first window (8m22s / 29,428 frames, normal exit) was never seen/played by the user. `build/evidence/human.profile` is **unattended attract**, not human matches. Its command was `./build/landmakr --profile-out build/evidence/human.profile --video-scale 2`.

The second window was visibly foregrounded and the user confirms playing single-player campaign for a while. Exact command:

```sh
./build/landmakr --profile-out build/evidence/human-play.profile --video-scale 2
```

Working directory `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile`.
Profile `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile/build/evidence/human-play.profile`.
Service `HumanProfilePlay2`, pid 74901; native sound, default CPU game video, scale 2, no time/frame limit and no collector early stop. Desktop proof `build/evidence/human-play-visible.png` (ignored).

It exited 1 on `Block profile write failed: build/evidence/human-play.profile.tmp`, not normal Escape completion. The preserved periodic snapshot has 25,244 keys (main 17,858, sound 7,386) / 266,141,218 hits and adds 9 main entries to the prior union. The unflushed tail / exact final frame count are unavailable. Disk was nearly full. A relative-destination cwd failure was separately reproduced/fixed, but the original window's precise cause was not recorded with errno. Destination paths are now startup-cwd stable and write/open failures include errno.

Frozen merge: eight seeded runs + unattended window + instrumented attract + genuine campaign snapshot. No held-outs, forced cold probes or crash-proof data merged.

## Local evidence and checkpoints

Ignored `build/evidence/` retains final-gates JSON, final-size JSON, held-out cold-hit table, regional compiler times, exact command logs, profiles, screenshots and golden WAVs. Some early evidence writes hit ENOSPC; inconclusive gates were rerun after freeing only experiment-owned reproducible C / verified duplicate WAVs. Previously observed real cold-abort logs were retained, not retrained or suppressed.

Generated C was measured then removed for storage; rerun CMake configure before rebuilding those directories or recomputing source attribution. Binaries, objects and inventories remain local. Throwaway smoke sources/executables were removed after proof; no shared/user artifacts deleted.

Local tags: `binsize-profile-1-instrumentation`, `binsize-profile-2-collected`, `binsize-profile-3-tiers`. Frontend changes remain isolated to profile option/session and strict-mode guards. ABI 2 and canonical game state remain unchanged.
