# Log

## [2026-10-09] init | wiki
Created SCHEMA.md, index.md, log.md and the layout directories.

## [2026-10-09] ingest | landmakrj round gate
Touched raw/tests/landmakrj-round-gate-watch.md, raw/disasm/landmakrj-0008e600-round-gate.md,
routines/landmakrj-0008e20c-round-task.md, games/landmakrj.md, games/commandw.md and index.md.

## [2026-10-09] ingest | 12Me21/taito-f3 hardware notes, MAME 0.289 F3 drivers
Added git submodule raw/docs/taito-f3 (https://github.com/12Me21/taito-f3 @ a070cbd) with
provenance sidecar raw/docs/taito-f3.md. Copied MAME 0.289 (git cfc4760a) taito_f3.cpp, taito_f3.h,
taito_f3_v.cpp, taito_en.cpp, taito_en.h to raw/emu-source/mame-0.289/ with raw frontmatter.
SCHEMA.md: added tags clip, reset, memory-map, pcb; documented submodule sources.
Created hardware/{board,clocks,cartridge,pal-decoders,fcm,memory-map,fdp,sprites,
sprite-command-word,tilemaps,pivot-layer,line-ram,line-ram-registers,fda,priority-and-blend,
clip-and-mosaic,highcolor-layer,shadow-mode,video-timing,interrupts,fio,sound}.md,
quirks/{frame-pulse-interrupt-window,interrupt5-timer-interval,video-blank-bit-invalid-sync,
sprite-enable-scrolls-value-2,sprite-set-scroll-affects-same-sprite,sprite-odd-bank-lasts-one-frame,
lineram-latch-persists-across-frames,tilemap-y-zoom-pf1-pf3-swapped,priority-conflict-shows-background,
shadow-mode-3-pivot-sets-bit12,mosaic-counter-resets-before-right-edge}.md,
comparisons/{hardware-vs-mame-video,hardware-vs-mame-system}.md; rewrote index.md.
All new pages are hypothesis. Notes-vs-MAME disagreements are recorded in each page's
`contradictions` and need tests here to resolve (clip enable polarity, blur polarity, int3 delay,
int5 timer, CPU clock 16 vs 15.238 MHz, palette RAM extent, pivot flip bits, tile flip bits).

## [2026-10-09] lint | 1 issue
hardware/shadow-mode.md linked the unwritten comparisons/hardware-vs-mame; repointed to
comparisons/hardware-vs-mame-video. No broken links, missing raw refs, unknown tags or orphans remain.

## [2026-10-09] lint | 46 issues
Deep lint of the 35 taito-f3 pages: quotes checked verbatim against the cited raw files, `Lnnn`
line refs checked against the raw copies (numbering includes the 8-line raw frontmatter; all
correct), MAME raw sha256 and submodule HEAD (no drift).
- 24 quotes not verbatim or cited to the wrong file: made verbatim, recited, or unquoted
  (board, cartridge, fdp, fio, interrupts, line-ram, line-ram-registers, pal-decoders,
  pivot-layer, priority-and-blend, sound, tilemaps, comparisons/hardware-vs-mame-system,
  comparisons/hardware-vs-mame-video).
- 15 list items/paragraphs without a per-line citation: cited (fda, priority-and-blend,
  shadow-mode, sprites, sound, memory-map, pivot-layer, quirks/lineram-latch-persists-across-frames,
  quirks/tilemap-y-zoom-pf1-pf3-swapped, quirks/sprite-enable-scrolls-value-2).
- 3 frontmatter address lists in lowercase hex: uppercased (memory-map, pal-decoders,
  comparisons/hardware-vs-mame-system). hardware/video-timing lacked `games`: added.
- quirks/video-blank-bit-invalid-sync had no wikilinks: linked hardware/fdp and hardware/video-timing.
- 2 pages over 200 lines: hardware/fdp terminology moved to new hardware/glossary.md; the redundant
  per-feature summary table removed from hardware/line-ram-registers.
Touched: the pages above, hardware/glossary.md, index.md, log.md.
Remaining: 30 of 36 pages carry contradictions (contested); all 36 are open hypotheses; intro,
legend and navigation paragraphs carry no citation by design.

## [2026-10-10] ingest | taito-f3 submodule bump a070cbd → eaa5a0a
Moved raw/docs/taito-f3 from a070cbd to eaa5a0a (“note on 5505-5510 channels”). The only change in
the range is the added raw/docs/taito-f3/5505-5510.txt (22 lines): 12's notes on the OTIS/ESP sound
hardware. Not ingested; no page cites it yet, and the bump changes no existing file, so no existing
citations need re-checking. Updated the provenance sidecar raw/docs/taito-f3.md: git_commit →
eaa5a0ab624464f5ae904a6227414d09d75f64ee, the git hash in captured_with → eaa5a0a, ingested →
2026-10-10. The submodule pointer is staged in the superproject and not committed.
Touched: raw/docs/taito-f3.md, log.md.
