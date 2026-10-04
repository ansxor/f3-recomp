DONE — Phase 7 survey checkpoint only (`gpuvideo-6-survey`, docs/data); executable is the Phase 6 baseline.

## Evidence-first survey

- Phase 6 committed/tagged first: `89aa284` / `gpuvideo-5-auto-scale`. Latest Phase 7 brief then read, including Y's clip/alpha/mosaic/zoom/rowscroll checklist and independently switchable palette-add requirement.
- Strict-native main/sound: seeds 5/6/7/41 ×40,000 plus 12,000 no-input attract =172,000 frames,170,845 semantic-supported,1155 startup oracle,zero fallback instructions. Every-frame field shares; sampled all256 rows of all9 layers including disabled/blanking inputs. Five additional result capture passes overlap54,600 frames, excluded from denominators.
- Actual title/attract/how-to/selection/played boards/WON-LOST captures and frozen replay snapshots retained. Campaign ending not reached; its explicit unsupported bitmap/slide producers are source-audited, not passed off as played coverage.
- PF0 table-wave source-X varies1352 frames (0.786%); PF2 perspective X-step/source-X/palette4381 (2.547%); shared alpha block variation5458 (3.173%). PF1/PF3 have no varying X/Y step, source-X or palette-add. All sampled enabled Y steps256,Y fractions0. No active animated mosaic, four-row active clip ramp or alpha ramp.
- The actual played diamond floor is **PF0 pre-drawn ROM perspective**, not PF2 water. Seed5 frame6000 isolated PF0 identifies it; PF1 is the architectural backdrop, PF2/PF3 empty. Entire4x frame and all isolated layers differ by zero pixels from nearest1x on that unit-scale frame. Inventing a new transform would not be recovering detail.
- PF2 water has two valid symmetric X-ramp halves, an eight-bit wrap boundary, discrete column-offset halves and palette RLE bands. PF0 sine uses packed ROM-table samples and map-period wrapping; do not substitute analytic sine.
- Clip edges move per frame but are constant in row blocks; alpha fades are uniform row blocks/frame-time changes; priority/mix/enable and mosaic are discrete. Full producer-address/field decision table in `docs/GPU-VIDEO.md`.
- 32,571,398 sprite records:259,072 zoomed (0.795%),zero fractional origins. Axis-aligned scale/flips only; ROM quantizes intermediate carry before submitting integer coordinates, no sprite rotation matrix. Existing off4x already changes frame1080 SP2/SP3 by2560/8832 pixels versus nearest1x, with zero newly sampled RGB values; unscaled frame6000 gains zero.
- Existing frame1500 palette baseline: linear invents339 active-palette-nonentry RGB colors over18,745 pixels (nearest native entry mean/max4.543/6.928 RGB units); fit895 over228,892 (3.880/6.928). Off has zero. Old fit also changes62,661 native subrow-zero pixels; Phase7's locally anchored fit must correct that contract.

## Checkpoint scope and limits

- This tag changes documentation/data only; no interpolation or sprite cutover is included yet. Runtime/canonical/audio behavior remains the committed Phase6 state, including the measured production cap4 and auto-scaling guarantees.
- `docs/GPU-VIDEO.md` records the survey, producer formulas, actual floor identity, field checklist, frame-share denominators, baseline gains and palette distances. `NOTES.md` has the requested Video results slot.
- All row/descriptor/frame CSVs, PNGs, pre-scanout snapshots and logs remain external under `/tmp/f3-gpuvideo/general`; no ROMs/generated blocks/binaries committed. Metal/Retina host only. Raw mode/phase words are recorded without falsely treating character/match values as a scene enum.
- Separate general-line and sprite implementation tags follow within Phase7; this checkpoint exists to compare the untouched baseline against those implementations.
