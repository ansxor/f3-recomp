DONE: full-coverage tiers pass all gates; slim is opt-in and rejected for general play (7/8 held-out cold aborts, 87.5%). Genuine campaign snapshot is partial after an output-write failure; no ending/exhaustive-gameplay claim.

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
