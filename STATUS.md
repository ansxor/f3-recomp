IN PROGRESS

# Phase 2 — Land Maker game-data video renderer

Worktree: `/Users/darien/Workspace/f3-stuff/f3-recomp/wt/video`, branch `video`, created from `checkpoint-1-coverage` (`46d0dad`). Runtime and integration worktrees are unchanged. No pushes.

## Contract

- Render from Land Maker Japan 2.01J's own display-building routines and game-owned main-RAM/ROM data, not a second graphics-RAM mirror.
- Keep `runtime/video.cpp` as the selectable FDP differential oracle.
- Record routine addresses, structure layouts, exercised features and evidence in `docs/VIDEO-HLE.md`.
- Establish exact pixel parity incrementally, first sprites or one tilemap layer, then remaining layers, using the committed seeded-gameplay harness with per-layer mismatch counts.
- Unsupported features must be explicit; an oracle fallback is permitted only for those frames/features and must retain the correct visible result.
- Preserve strict-native main CPU execution with zero fallback and 25/25 sampled attract-frame matches against MAME in oracle mode.
- Only after base pixel parity: internal resolution scale, widescreen/extra border and optional filtering, all off by default.

## Current evidence

- Read `prompts/video.md`, `CONTEXT.md` and `GOAL.md`.
- Worktree creation reports HEAD `46d0dad`; checkpoint coverage/native/video acceptance is inherited evidence, not a fresh phase-2 verification.
- Existing hooks invoke a C callback at a discovered instruction boundary after flags are materialized; the callback can observe game registers/data without skipping native code.
- Existing oracle renders from FDP palette/graphics/control RAM and delays sprite state by one frame. Its renderer is retained.
- First component implemented: game descriptor-driven PF0 semantic tilemap, with source-layer pixel comparison against the unchanged FDP renderer.
- Seeds 5, 6 and 7 × 3600 strict-native frames: 26 PF0 samples per seed, 40,894,464 indexed pixels total, zero mismatches and zero main-CPU fallback instructions. Domain is the complete 1024×512 logical tilemap, not yet final composited RGB.
- The first run rejected a real uncovered selection-screen producer at frame 1320, PC `$09ec66`. Adding the high-level side-strip hook `$09ec4e` made the same scenario pass.
- Routine/layout evidence and the initial exact counts are in `docs/VIDEO-HLE.md`. Sprite and line components are being integrated; no full HLE frame-parity claim yet.
- All four playfield texture planes now pass seed 5 × 6000 frames: 46 samples and 24,117,248 indexed pixels per layer, each zero mismatches; 80,338,232 native blocks and zero fallback. `f3rt-check` also passes. A PF1 reversal defect exposed Capstone's omitted printed ×4 index scale; the raw `$14fc` extension supplied the correction.

Evidence priority: actual ROM behavior and observed Land Maker output, then primary hardware sources, work-in-progress notes, then MAME source. Conflicts are documented rather than silently chosen.

## Verification still required

Remaining layers and final game-data compositor, full per-layer seeded-frame comparisons, unsupported-feature accounting, fresh oracle/MAME/native regression, and actual renderer/presentation surface verification. Existing phase-1 accepted audio timing limits remain unchanged; no audio work is in scope.
