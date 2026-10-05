BLOCKED: all combined automated gates pass; foreground human window/normal Escape exit pending. Limits: landmakrj only, finite exclusion/profile coverage, Metal only; slim remains rejected for general play.

# Combined binary-size cutover

## Build and default

- Semantic merge: integration + `binsize-exclude` + `binsize-profile`, ABI **3** on both CPUs. Six config exclusions unchanged; tier assignment uses only their complement. GPU/automatic-scale/audio-stall behavior retained.
- ROM-generated `landmakrj` defaults to full-coverage hot `-O2` / cold `-Oz` (`-Os` outside Clang). Opt out with `F3_PROFILE_DEFAULT_TIERS=OFF` and clear any explicit `F3_PROFILE_TIERS` cache override. `F3_PROFILE_TIERS=<file>` overrides the frozen profile; slim stays explicit.
- Combined `build/landmakr`: **31,525,032 file bytes**, **18,857,984 __TEXT**, **175,404,686 generated C**, **50,629,824 generated objects**. **336,775 main / 61,997 sound entries** retained; hot **30,146 / 7,519**. File / __TEXT **62.664% / 66.859% smaller** than the exclusion experiment's 84,436,984 / 56,901,632-byte baseline. Successful configure/build **8.028 / 27.619 s**, `-j 6` two targets.
- Combined+slim: **6,023,976 file / 4,587,520 __TEXT bytes**, **19,120,803 C / 6,122,760 objects**, **30,146 main / 7,519 sound entries**; configure/build **3.520 / 6.086 s**. Not a general-playability recommendation.
- Full five-row baseline / A / B-tier / combined / combined+slim table, regional attribution, build-time/provenance qualifications and commands: [docs/BINSIZE-COMBINED.md](docs/BINSIZE-COMBINED.md).

## Frozen corpus and enforcement

Original seeded 101–108 + unattended attract + instrumented attract + genuine user-confirmed partial campaign inputs re-merged **byte-identically**. SHA256 `5ae3c7b6e01b07a569ab984a7409cce33ab13282090435188ad65cb61ccffebb`; **0 excluded hit/miss records**, **0 misses**, 30,146 main / 7,519 sound hit keys. Held-outs 301–308 were not profiled or merged. The original human profile's output-write-failure tail remains unavailable; no ending/completed-match claim.

Both generators reject contradictory excluded profile records. ABI 3 exclusion errors precede interpretation and slim cold-hit handling, including odd/physical aliases and main trace mode. Actual full **and** slim probes each pass **48 main / 12 sound failures**, **3 half-open boundaries**, zero fallback/opcode progress. Real retained cold NOPs execute natively; opt-in slim cold misses durably record address/CRC and fail. Frontend `--allow-fallback` retains zero-fallback native execution in full tiers; slim rejects the flag. Opt-out actually runs 600 native frames with the same entry/exclusion set.

## Combined automated gates

| Gate | Exact observed result |
|---|---|
| Fresh MAME attract | **25/25**, frames 600–3480 every 120; **1,856,000 RGB pixels**, **0 differences / 0 maximum channel error** |
| Frame-600 RAM | **131,072 bytes exact** vs fresh MAME capture |
| Attract audio | **3,600 frames**, **7,270,664-byte WAV identical** to supplied baseline; SHA256 `62bac3a4d1baef4e7c2343ce987e44eb654a71f90d653b5b88c1d12d15eff05b` |
| Native attract counters | **51,507,335 blocks**, **0 CPU fallback**, cycles 977,201,324, 1,817,655 audio frames, frame CRC `3359f200` |
| Native video invariant | **250,114,560 RGB checks**, **0 differences**; explicit video-oracle presentation fallback does not enable CPU fallback |
| Held-outs 301–308 | **8 × 20,000 = 160,000 frames**, **8/8 success**, **8 identical WAVs / 56 identical final files**, **2,147,127,680 native blocks**, **0 fallback / 0 cold aborts** |
| GPU Metal harness | **35/35 success**, **137,600 native frames**, **4,669 composites / 39,747 isolated-layer checks**, **0 mismatching pixels / 0 CPU fallback** |
| GPU transitions | Six induced scenarios / 18 sample groups, 3 native sprite-boundary branches, 21 interpolation boundary checks, 349 sprite checks, **9 actual scale changes**; canonical/native/audio/restore/trail invariants exact |
| Go relay | `go test ./...` **PASS**, `f3rt/netplay/server` 0.423 s; command wall 0.938 s |
| Save/load oracle | Seed 301, native+oracle sound × 6,000 frames: **60/60 exact replays**, **0 save / 0 load allocations**, 60 wall-clock perturbations |
| Synthetic / runtime | **45/45 Python checks**, actual `f3rt-check` **PASS**, every main/sound caller built |

GPU matrix: seeds 5/6/7/41 × scales 1–4 × borders 0/48 × 4,000 frames, plus linear/fit geometry and fit boundary/scale transitions; sampled `--every 30 --layers`. Metal only; induced ending is not a played ending. Snapshot state sizes native/oracle **4,231,509 / 4,231,724**, final CRCs `bcdefd0a` / `bc62613f`; both frame CRC `177cbcda`, audio CRC `1f4a6e2b`, 3,029,425 audio samples. Existing snapshot oracle, not a new impaired-network campaign.

## Human validation

The requested unbounded, windowed command will run only after the gated checkpoint:

```sh
./build/landmakr --video-backend gpu --video-scale auto-integer --video-border 48
```

No automated match inputs or collector early-stop. Command, actual duration, normal exit/abort/crash evidence and visibility will be recorded here after the user closes the foreground window.

## Checkpoints and evidence

- `binsize-combined-1-merged`: semantic merge builds/basic native checks passed.
- `binsize-combined-2-gated`: all automated gates above; human run is the remaining admission step.
- Full evidence: ignored `build/combined-evidence/automated-gates.json`, `profile-audit.json`, `sizes.json`, `attract-gates.json`, `heldout-gates.json`, `gpu/results.json`, `netplay/results.json`, exact command logs and per-seed hashes/state dumps. Original baseline artifacts unchanged. Verified duplicate candidate WAVs and throwaway probe sources/binaries removed; no user/shared artifacts deleted.
- No pushes; no ROMs, binaries, generated C or captures committed. README/site build sections and all BINSIZE reports updated.

## Limits

Finite seeded coverage is not an all-state exclusion proof. Full coverage retains all existing nonexcluded entries, not every baseline unsupported lowering. Only Japanese Land Maker on this Darwin arm64/Metal host is validated; no other set/GPU or played campaign ending claim. Original campaign snapshot is partial. Slim's historical held-out abort rate remains **7/8 (87.5%)** and it is not the default.
