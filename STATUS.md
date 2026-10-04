BLOCKED: final tier/slim rebuilds and parity gates are pending; the genuine campaign snapshot is merged, with an unflushed tail lost after an output-write failure.

# Binary-size experiment B — execution profiles

Strict native main and sound CPUs. No interpreter fallback, pushes, ROMs, generated code or binaries committed.

## Instrumentation checkpoint

`binsize-profile-1-instrumentation`: version-1 profiles keyed by CPU, ROM CRC32 and actual entry address; every main label (including intra-page fallthrough) and sound entry, with shared exceptions counted once. Saturating uint64 increments allocate nothing and compile away without `F3_PROFILE_INSTRUMENT=ON`. `--profile-out FILE` merges sequential runs, rejects concurrent writers to one path, and atomically flushes/fsyncs every 30 wall-clock seconds and at exit. Relative destinations are now fixed against the startup cwd; write failures now include errno.

- Main original entries: 1,033,276 (464,523 decoded instructions plus 568,753 proven exception entries). Sound: 262,144.
- Configure 16.78 s; initial `-j 6` instrumentation build 117.12 s; saturated-counter rebuild including configure 232.58 s. Timings reflect host load.
- Two deterministic 600-frame runs exactly doubled all 4,425 entry counts; frame-600 main RAM equals fresh MAME.
- Baseline and instrumented attract: 25/25 frames, 1,856,000 pixels, zero RGB differences; RAM600 exact; instrumented attract WAV byte-identical to baseline.
- Training seeds 101–108 × 20,000 frames: 8/8 success, zero fallback, WAV and seven final state files byte-identical to baseline for each seed.
- Held-out reference seeds 301–308 × 20,000: 8/8 success. Excluded from profile collection.
- Crash preservation: separate headless profiling process SIGKILL at 35.051 s; periodic file retained 18,561 entry keys / 272,208,868 hits. Human windows were not stopped by the collector.
- Active-path writer exclusion verified. 21 unique passing Python/compiled-C/C++ regressions, including a redirecting hook entry and cwd-independent persistence. Runtime device executable: PASS.

## Human collection — corrected provenance and limits

The first window ran 8m22s / 29,428 frames and exited normally, but the user reports never seeing or playing it. `build/evidence/human.profile` is **unattended attract data**, not human gameplay.

The second window was visibly foregrounded. The user confirms playing the single-player campaign for a while:

```sh
./build/landmakr --profile-out build/evidence/human-play.profile --video-scale 2
```

Working directory: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile`.
Profile: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile/build/evidence/human-play.profile`.
Service `HumanProfilePlay2`, pid 74901. Native sound, default game video, no frame limit, no collector early stop. Desktop evidence: `build/evidence/human-play-visible.png` (ignored).

The process exited 1 after an output flush failure, not a normal Escape completion: `Block profile write failed: build/evidence/human-play.profile.tmp`. Its valid periodic snapshot preserves 25,244 entry keys (main 17,858, sound 7,386) / 266,141,218 hits. The unflushed tail and exact final frame count are unavailable; no claim of completed matches/endings. Disk was nearly full. A separate real-runtime driver reproduced the same failure by changing cwd; anchoring the destination fixed that regression. The original window's actual failure cause was not captured with errno and is not proven.

Original unseen command, retained only as additional attract data:

```sh
./build/landmakr --profile-out build/evidence/human.profile --video-scale 2
```

## Frozen collection

`profiles/landmakrj.profile`: eight training runs + unattended attract + 3,480-frame instrumented attract + the genuine campaign periodic snapshot. 1,186,177 bytes; main CRC `15a59a08`, sound CRC `5a7e9117`; addresses/counts only, no ROM bytes. The genuine snapshot contributes 9 previously unseen main entries. Held-out seeds and crash-proof data are not merged.

| Descriptive region | Ever executed / original entries |
|---|---:|
| Main code bin | 28,817 / 290,024 |
| Main homogeneous padding | 0 / 486,966 |
| Main data/gfx-looking bin | 1,329 / 256,286 |
| Sound program | 7,519 / 262,144 |

Main total 30,146 / 1,033,276; sound 7,519 / 262,144. Gfx-looking contains real executed entries; entropy is not a safe discovery exclusion proof. These bins are descriptive only.

## Candidate gates pending

Full baseline, full-coverage `-O2` hot / `-Oz` cold tiers, and explicit opt-in slim use `build/full`, `build/tiers`, `build/slim`. Provisional builds are being regenerated against the final profile and cwd fix before size/safety acceptance. Initial fresh full-baseline attract already passed 25/25 MAME, RAM600 and WAV parity.

Reference `/Users/darien/Workspace/f3-stuff/f3-recomp/build/landmakr`: file 84,291,656 bytes; __TEXT 56,770,560; __text 54,706,200. Generated C: main 261,098,018 + sound 55,210,095 = 316,308,113 bytes. Initial instrumented file 105,856,312; __TEXT 78,102,528; generated C 337,607,288.

Local logs/JSON under `build/evidence/` retain configure/build times, MAME comparisons, collection provenance and source-region measurements. Reproducible experiment-generated C was removed after measuring it to recover disk space; final builds must regenerate it. No shared/user artifacts were removed.
