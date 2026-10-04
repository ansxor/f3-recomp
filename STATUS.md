DONE — Phase 7 general line sampling checkpoint `gpuvideo-7-general-lineram`; sprites unchanged at this checkpoint. Geometry defaults on only within opt-in interpolation; palette separately opt-in, alpha/clip/mosaic/priority/column jumps native/discrete. Metal exercised; no other GPU or played campaign ending claim.

## Delivered

- Replaced the water-only recognizer and absolute fits with independent valid-run analysis for every playfield/field. PF0's sampled coordinate wave and both valid PF2 zoom/centering halves now interpolate. Linear and locally anchored shape-preserving fit preserve every native subrow-zero sample and every unflagged row exactly.
- Separate `--video-interp-fields none|geometry|palette|geometry,palette`, default geometry. Same-pen RGB bank blending requires compatible strides and actual ROM pen/endpoint-footprint checks; unsafe palette leaves safe geometry enabled. No guessed sine, alpha ramp, clip ramp, vertical zoom ramp, fractional sprite origin or rotation.
- Appended per-field metadata exists only in GPU upload storage; canonical scene/snapshots and CPU pixels are unchanged. Storage and staging allocations both include the entire append. Off retains its original allocation/path.
- Current CLI/help/README/site/ABI/GPU design docs and NOTES Video slot updated. Earlier phase 5 water measurements remain labeled historical. No pushes or ROM/generated-program/capture/helper files committed.

## Exercised proof

- 56 cases /224,000 native frames:3112 complete off GPU/CPU comparisons and2544 in each of nine isolated layers, zero differences. Off scales1–4/borders0,48; linear/fit geometry/palette/both/none, seeds5/6/7/41.2248 opt-in native/unflagged/text/sprite checks,252 visible-ROM boundary guards,136 runtime scale changes.
- Same-seed native cycles/blocks/audio frames/audio CRC/final native RGB CRC match across every option. Same constructor geometry keeps exact canonical snapshot bytes; host scale transitions do not change those bytes. Independent ordinary CPU-backend replay matches each final frame.
- Induced bitmap/trails/global-flip/unknown-writer/ending-producer fallback and recovery remain exact; not a played ending. Zero CPU fallback instructions.
- 4x captures: PF0 wave1300 changes49,635 linear/49,558 fit pixels; water1500 changes85,792/86,231 geometry-only pixels with zero non-palette RGB outputs. Palette+geometry adds339 colors in18,745/18,746 pixels; nearest-native RGB mean4.543/3.407, max6.928. Captures do not justify palette-on default. Floor6000/attract-alpha2400/results4850 gain0, for measured reasons.
- Isolated100-repeat frame1560/border48 mean/p95/worst ms:4x CPU21.496/29.055/30.559; GPU off2.008/2.294/2.434; linear geometry2.119/2.418/2.479; fit geometry2.162/2.434/2.527; fit+palette2.169/2.451/2.590. Full1/2/4 tables and wave timing in docs/GPU-VIDEO.md; includes GPU fence/readback, not kernel time.
- Actual Cocoa/Metal key-event windows off/linear/fit keep matching native CRC/cycles/blocks and byte-identical WAVs; internal/window surfaces inspected. Paced automatic-integer fit geometry at2496x1392 physical pixels/internal1664x928 has audio mean27.984/max37.499ms,zero queue drops; four clock resyncs include screenshot stalls.
- Fresh3600-frame headless compare retains250,114,560 exact native RGB checks and byte-identical established WAV. GPU-off frontend/runtime-device check build/run passes. Existing native/MAME25/25 acceptance unchanged; no new MAME capture comparison claimed.

## Evidence and limits

- Commands, metrics and artifacts: docs/GPU-VIDEO.md; `/tmp/f3-gpuvideo/general/{parity,effect-runs.json,bench-runs.json,windows}` and frozen effect directories outside the repo.
- Static/predrawn floor has no live geometric transform to interpolate. Actual line alpha/clip/priority/column controls are discrete blocks; no smooth Y zoom or mosaic ramp observed. Sprites retain phase 6 sampling until the separate sprite checkpoint; ROM contains no fractional origin or rotation matrix.
- Headless/native captures, CRCs, WAV and canonical rollback state remain CPU-produced. Netplay still fixed scale1/border0. CPU and interpolation-off remain defaults. Other GPUs/second monitor and played ending unexercised.
