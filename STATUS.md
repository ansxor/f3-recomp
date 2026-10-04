DONE

# Phase 2 — Land Maker game-data video renderer

Worktree: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/video`, branch `video`, based on `checkpoint-1-coverage` (`46d0dad`). Runtime/integration worktrees are unchanged. Local commits only; no pushes.

## Implemented

- Japan 2.01J scene reconstruction from game ROM/main-RAM descriptors and native producer hooks: four playfields, four sprite priority groups, programmable text/glyphs, scroll/zoom, line priorities, alpha and clipping. No FDP geometry/write-value mirror.
- Independent selectable oracle retained in `runtime/video.cpp`. `--video fdp|game|compare`, default `fdp`; game modes require strict native execution. CPU ABI remains version 2.
- ROM addresses, structures, observed features, source conflicts, commands and scoped limitations documented in `docs/VIDEO-HLE.md`; frontend usage in `README.md`.
- Post-base-parity presentation: internal scale 1–4, extra border 0–160 native columns per side, nearest/linear SDL filtering. Defaults are scale 1, border 0, nearest. Native captures/CRCs remain 320×232.

## Exercised evidence

- Incremental PF0 first: seeds 5/6/7 ×3600 frames, 40,894,464 indexed pixels total, zero mismatches and zero CPU fallback. Followed by independent four-PF, text and sprite-group proofs.
- Complete base gate before presentation work: seeds 5 and 6 ×6000 frames; all nine source layers, normalized visible row descriptions and final RGB pass 46 samples per seed.
- Extended complete scenes: seeds 5/6/7 ×40,000 frames, 329 samples per seed, zero mismatches in every layer and 24,424,960 final RGB pixels per seed. Native blocks: 534,492,746 /529,768,165 /536,649,307; CPU fallback zero. Only 231 startup renderer fallback frames, last at frame 418.
- Final continuous seed-5 check, every frame 600–4000: 3401 samples; each PF 1,783,103,488 pixels, each sprite group 252,490,240, text 891,551,744 and composite 252,490,240, all zero mismatches. 54,087,373 native blocks; CPU fallback zero.
- Continuous attract comparison through 3480: 3249 HLE frames /241,205,760 RGB pixels exact; only 231 startup fallbacks. Ordinary attract profiles are reconstructed, not hidden behind fallback.
- Fresh oracle **and game-renderer** attract captures each match **25/25 MAME frames**, 1,856,000 RGB pixels per run, zero mismatches/max channel error. Each executes 49,866,062 native blocks, CPU fallback zero, final CRC `b490d7d9`.
- Two-machine seed-5 presentation smoke: scales 1/2 with border 48, 2400 frames each, 178,176,000 native RGB pixels per run exact; PC/cycles/D/A registers equal. At 1× the native center is unchanged. Off-screen scene content reaches all 22,272 added pixels; 2× rerasterization differs from RGB enlargement in 5,450,015 pixels across the run.
- The same smoke injects an unknown PF0 writer after sustained game rendering: precisely eight fallback frames, exact native image and black extra columns. No CPU fallback.
- Scale 3/4 with maximum border 160: two-machine 1500-frame runs, 1920×696 /2560×928 internal images, 111,360,000 native RGB pixels per run exact, CPU fallback zero. Both produce real off-screen content and subpixel rerasterization.
- Actual Cocoa/Metal surfaces visually inspected at scale 2 /border 48, nearest and linear. Filter selection changes 172,330 RGB surface pixels while native CRC/CPU execution stay equal. Final enhanced frontend runs through attract frame 3480 with the same native CRC and zero CPU fallback.
- `f3rt-check` passes. Permanent regressions cover mirrored grid coordinate quantization and PF descriptor flip/palette/coverage. Nominal sprite clipping before texel rounding and unreadable-descriptor rejection additionally have observed failing-before/passing-after regression runs.

## Explicit limits

Startup/POST, ending line transitions, bitmap pivot, global screen flip, retained sprite trails, unknown producers and unsupported descriptor ranges use the correct oracle picture rather than invented geometry. Known complete initialization restores component ownership. Enhanced unsupported frames are centered with black added columns. Ending/flip/trail behavior is not claimed as HLE-validated; mosaic decoding is present but no nontrivial mosaic animation is claimed as exercised. Extra border does not rewrite game logic/HUD or manufacture off-screen artwork.

The Furrtek die page was inaccessible (403/Cloudflare); no unread claim is used. ROM behavior/observed output outrank WIP notes and MAME implementation details. The sprite nominal-rectangle edge cull is explicitly a retained-oracle compatibility rule, not an unmeasured physical-chip claim. Existing accepted audio timing limits are unchanged; audio parity work is out of scope.

## Delivery

Implementation and exercised acceptance are complete, with the explicit limits above. Local milestones: `ee76002` isolation, `08a5178` first PF0 proof, `7e5ff57` four-playfield parity, `09cd6a8` text/sprite descriptors, `8924ea5` complete base scene parity, `8d33ccd` attract profiles, and `c011048` verified post-parity presentation and safeguards. No pushes or other-worktree edits. Throwaway smoke source/executable were removed; generated captures remain ignored evidence.
