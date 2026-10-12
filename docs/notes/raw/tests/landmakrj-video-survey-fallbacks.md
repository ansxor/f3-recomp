---
source_url: docs/developer/GPU-VIDEO.md + VIDEO-HLE.md
ingested: 2026-10-11
sha256: 4a2a7bb9fdf68dc04c4622dd9904887b035d494b777f6f1ae2b764a85d5a4121
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository record of runs (secondhand: command output and artifacts under ignored build/ were not retained); source doc deleted after ingest; f3rt-tool gameplay/gpu-compare surveys, seeds 5/6/7/41"
---
<!-- excerpt: docs/developer/GPU-VIDEO.md lines 205-225 -->
## Port invariant and fallback evidence

Native output is still composed on the CPU at every VBSTART. GPU export occurs
before `latch_sprites`, so its sprite list is the list rendered in that frame,
not the following list. GPU resources/caches are absent from canonical state.
CPU-only presentation remains the default; GPU selection is explicit.

An independent CPU-backend snapshot branch exposed an expanded trails history
bug: deferring every expanded sprite raster lost intermediate retained sprites.
Seed 5/frame 3404, four induced trail lists with changing scroll, failed before
the correction with **213,384 differing canonical bytes**. The fix seeds the
preceding expanded plane on entering retention and keeps rasterizing while
trails are latched. The same regression now has **zero differing bytes**.
Trail frames remain exact native oracle presentation, not unsupported HLE.

Seed 5 through 3600 frames additionally induced actual bitmap, trails and flip
fallback boundaries at frame 3404, restoring
and replaying the original native frame after each branch. All three fallback
images and supported recoveries match CPU output; canonical restore is exact.
The original 231 startup fallback frames remain unchanged.
Isolated-layer counts exclude fallback images, since fallback bypasses masks.

<!-- excerpt: docs/developer/GPU-VIDEO.md lines 734-768 -->
### Corpus, row dumps and actual scene coverage

Strict native main/sound, canonical scale 1/border 48, selected GPU scale 4:
seeds **5/6/7/41 ×40,000** plus **12,000 no-input attract frames** =
**172,000 frames**, **170,845 supported**, **1155 startup oracle frames**,
zero fallback instructions. No frame/time/state manipulation in these routes.
Count field variation on every enabled visible row of every frame. Dump all
256 rows of PF0–3, SP0–3 and text every 120 frames and at selected captures,
including source phases/steps, palette add, priority/blend/clip selectors,
four raw clip planes, four saturated alpha weights, mosaic and bitmap state.

An additional five replay/capture passes total 54,600 overlapping frames,
excluded from the share denominators. Actual captures include title/attract,
how-to, game/character selection, several played match backgrounds and
**WON/LOST** transitions: seed 5 frame 4850, seed 7 frame 4250 and seed 6
frame 14030 were visually inspected. Late captures return to the attract demo,
not a played campaign ending. Ending hooks are audited below, but no campaign
ending is reached in these routes.

Artifacts stay outside the repository:

- `/tmp/f3-gpuvideo/general/survey-runs.json`, `survey-summary.json`,
  `{seed5,seed6,seed7,seed41,attract}.log`.
- `survey-data/<route>/{frames,rows,sprites,captures}.csv`: exact every-frame
  field masks/counts and sampled per-row/descriptor dumps. The row CSV includes
  disabled and blanking rows; they are **not** counted as valid effect inputs.
- `survey-data/<route>/frameN.{off4.png,native.bmp,before.state}`: original
  4x surfaces, native captures and untouched pre-scanout snapshots for replay.
- `results-data/<route>` and `results-runs.json`: targeted actual result frames
  with the same per-layer row dumps and replay snapshots.

Work-RAM words at `$401f54/$401f5c` are recorded as **raw mode/phase**, not
trusted human scene labels. Values 2/3/4/7 occur in visibly played matches;
`$ffff` occurs in no-input demos. Calling these a simple charsel/game enum
would mislabel the census. Captures and producer/field signatures label effects.

<!-- excerpt: docs/developer/VIDEO-HLE.md lines 210-219 (acceptance) -->
## Acceptance

The scoped phase is complete: VRAM-owned scene reconstruction at VBSTART,
per-game `games/<game>/video/` decoders, opt-in PC/address-only write logging
(`--discovery-log`), explicit `flipped-screen` / `sprite-trails` /
`bitmap-pivot` fallbacks and the opt-in presentation options are implemented.
`f3rt-tool gameplay --frames 6000 --video-diff --video-diff-every 60`
reports pixel_mismatches=0 for all nine layers and the composite over 91 sampled
frames, with `VIDEO game_frames=6000 oracle_fallback_frames=0` and the
pre-change frame CRC.
