BLOCKED: human gameplay profile and both size/safety evaluations are pending; instrumentation is verified.

# Binary-size experiment B — execution profiles

Worktree-local experiment; strict native main CPU and sound CPU, no interpreter fallback. No pushes, ROMs, generated code or binaries committed.

## Instrumentation checkpoint

- Optional `-DF3_PROFILE_INSTRUMENT=ON`, enabled by `--profile-out FILE` in `landmakr` and the seeded gameplay harness.
- Every generated executable address is instrumented at actual execution, including intra-page main fallthrough, indirect dispatch entries and shared main/sound exception handlers. Counts use dense address-indexed arrays; no hot-path allocation and no instrumentation when the build option is off. Counts saturate at uint64 max.
- Version 1 ASCII `F3-BLOCK-PROFILE` records ROM CRC, CPU, base, length, address and hit/miss counts, with no ROM bytes. Sequential runs merge in place; a nonblocking advisory lock rejects concurrent writers to the same output. Atomic replacement plus fsync occurs every 30 wall-clock seconds at frame boundaries and on exit. Separate files can be unioned with `tools/block_profile.py merge`.
- Original inventories: main 1,033,276 registered executable entries (464,523 decoded instructions plus 568,753 proven exception entries); sound 262,144 aligned entries. These are entry addresses, not merely 17,534 main packed functions.
- Release configure: 16.78 s. First instrumentation build (`landmakr`, gameplay harness, runtime check, `-j 6`): 117.12 s. Saturating-counter rebuild including reconfigure: 232.58 s.
- Actual frame-600 smoke: zero main instruction fallback; frame-600 main RAM byte-identical to fresh MAME. Two deterministic 600-frame runs produced exact count doubling at all 4,425 executed entry keys (main 1,467, sound 2,958).
- Compiled deadline/overlap/discovery regressions: 13 passing. Profile format/identity/count and compiled hot/cold/slim/shared-exception scenarios: 6 passing; saturation scenario additionally passing. Runtime device executable: PASS.
- Periodic-flush crash proof: a separate full profiling process was killed with SIGKILL at 35.051 s. The previously atomic-flushed file retained 18,561 executed entry keys and 272,208,868 hits; the human window was not touched.

## Human collection (launched; awaiting user Escape exit)

```sh
./build/landmakr --profile-out build/evidence/human.profile --video-scale 2
```

Profile: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/binsize-profile/build/evidence/human.profile`.
Default game video, native sound, windowed, scale 2. Exact command above launched as `HumanProfileWindow`, pid 32163; direct window capture verifies the rendered attract tutorial. A pre-existing GPU-regression crash-report dialog covers the desktop; the user was notified. The process must not be stopped early.

## Automated collection / references

- Training seeds: 101–108 × 20,000 frames, strict-native headless harness, native sound; per-run profiles and WAV/state evidence under `build/evidence/training/`.
- Held-out seeds: 301–308 × 20,000 frames, excluded from the committed profile; abort rate and WAV/state parity evaluated separately.
- Baseline executable: `/Users/darien/Workspace/f3-stuff/f3-recomp/build/landmakr`; 84,291,656 file bytes, __TEXT 56,770,560 bytes, __text 54,706,200 bytes.
- Baseline generated C: main 261,098,018 bytes, sound 55,210,095 bytes.
- Fresh MAME captures: 25 frames, 600–3480 step 120, separate cold-boot NVRAM/config under `build/evidence/mame-*`. Captures complete; comparison against candidates is pending.

## Pending evaluations

A. Full coverage: hot subsets `-O2`, cold subsets `-Oz` (Clang; `-Os` on GCC), separate units. No removal and no fallback.

B. Explicit `-DF3_PROFILE_SLIM=<profile>`: sparse retained main/sound tables; removed code aborts with address, ROM CRC, re-profile hint and a durable cold-hit record. No interpreter path may cover a removed entry.

Final report must contain merged human+seeded coverage by descriptive ROM region, file/__TEXT/generated-C size, configure/build timings, every acceptance gate, held-out abort rate, and limits. Region classifications are descriptive only, never an exclusion proof.
