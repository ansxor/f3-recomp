# Log

## [2026-10-16] fix | Land Maker 0x56 & 0x53 percussion click transients, sample-end interpolation and release-time voice stealing
Resolved transient clicks occurring in HLE voice playback (`runtime/audio/hle/synth.cpp`). 1. Non-looping sample end boundary: when a voice approached its end boundary, linear interpolation evaluated `sample(index + 1)` past `v.end`, reading unrelated attack data or headers from adjacent ROM instruments (e.g. sample 4360326..4368096 reading word 4368096 = 0x4500), producing an audible single-sample spike up to +453 immediately before voice stop. Clamping boundary lookup for non-looping samples smoothly fades to zero, eliminating all 62 percussion clicks in 0x56 (62 to 0). 2. Voice steal ordering: in `allocate()`, voices were previously selected using `v.serial` (assigned at note-on), meaning a long-held sustained note released 1 ms ago was prioritized for eviction over silent voices released hundreds of ms ago. Tracking `v.release_serial = ++serial` at `release()` and ordering released voices by `release_serial` ensures the oldest-released, quietest voices are reclaimed first, matching the real driver's doubly-linked `$d850` release queue. Added GoogleTest `HleAudio.NonLoopingSampleEndBoundaryDoesNotSpike` in `games/landmakrj/tests/audio.cpp`. Click counts: 0x56 drops from 62 to 0 (ref 65); 0x53 drops to 35 (ref 35). Kirameki 0x187 PCB reverb metrics unaffected (C50 diff +.22/+.20/+.20 dB, floor -.33 dB, L/R .720, tail_mae 2.99 dB). Touched: `runtime/audio/hle/synth.cpp`, `tools/commands/sound_extract.cpp`, `games/landmakrj/tests/audio.cpp`, `docs/notes/raw/tests/2026-10-16-landmakrj-clicks.md`, `docs/notes/log.md`.

## [2026-10-16] fix | Effects: one graph per decoded ES5510 program, selected from the ROM pointer table
Preset rows name their program in byte 5 (pointer table 0xc1c3a2); p0..p9 are row bytes 6..15 forward (the old reverse read was wrong). Decoded with esp_graph.py every program the three games reach: 0 Reverb (Command War preset 1), 1/2 Hall (Kirameki presets 0/2, two cross-coupled chains), 4 Mute (single END, silent), 5 Chorus, 6 Echo (Land Maker preset 11: ping-pong echo + enhancer; the old 274.8 ms echo was this program, not program 5); the other programs throw from Effects::select. Parameter mapping per program read by patching preset rows in a ROM copy and diffing GPRs (angle = table(8n+4), LFO rate 196.608*(p2-15)/2^23, Hall a = (p0+7)/128, ...). verify vs reference impulse -71..-75 dB per program; C++ vs Python models -67..-152 dB; Kirameki 0x187 PCB metrics unchanged. Tests: Echo/Chorus/Reverb/Hall/Mute/selection tests replace the program-5-only ones. Touched: runtime/audio/hle/effects.hpp, runtime/tests/audio.cpp, tools/compare/esp_graph.py, raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md (section 9), raw/tests/2026-10-16-esp-program-param-sweep.py.txt, log.md.

## [2026-10-16] fix | Kirameki 0x187 effects rebuilt from the decoded ES5510 program 5 graph
Decoded the loaded program with a new Python port of es5510.cpp (tools/compare/esp_graph.py: disasm, symbolic graph, verify). The earlier 6180-sample tank was wrong: delay RAM is one 64K ring, delays are (read - write offset)/256, the real loop is a 1105-frame return with nested all-pass stages (168/276/755/549/1369/1012/696/242 plus an LFO-modulated 921+-8) and a 0.37 DC loop gain; pair 2 is a 4-tap chorus (p2 = LFO rate), not an echo. The HLE graph in effects.hpp reproduces the emulator impulse to -77 dB re peak. 0x187 vs PCB: C50 +0.23/+0.20/+0.20 dB (was +0.30/-0.40/-0.57), floor -0.36 (+1.08), L/R corr 0.721 (PCB 0.713, was 0.826), side/mid -7.85 (-7.74, was -10.17), rms -0.93, tail mae 2.10 bias -0.12; code 1 low-confidence alignment, L/R 0.806 vs PCB 0.562 not improved (reference 0.918). Reference "plateau" is the ESP's 16-bit truncation DC (-51/-44 LSB at the output), not a decay failure; es5510.cpp untouched. Constants removed (K_DECAY_PER_P0, r_norm, comb tables); only p6-low-nibble mix angle and p2 LFO rate are fits to two driver writes. Only program 5 is decoded: landmakrj/commandw presets approximated. Tests: Audio.EnhancedEffectsRoutingMatchesHardwareWiring (echo check replaced by chorus), ReverbSendGainAndTapTimingMatchDecodedGraph, ReverbDecayFollowsP0 replace the pinned-constant test. Touched: runtime/audio/hle/effects.hpp, runtime/tests/audio.cpp, tools/compare/esp_graph.py, raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md (old section 7 withdrawn, section 8), log.md.

## [2026-10-16] fix | Kirameki 0x187 reverb drive and pair 2 path (effects.hpp)
Restored the reverb send normalisation 1/(1+cross), removed the pair 2 send low-pass and 2/3 dry attenuation, refit the tank decay to 0.63/40 against both PCB recordings (0x187 level -0.92 dB, tail 1.74/+1.46; sound 1 level +0.22 dB, tail 2.38/-2.28; was +1.47 dB). Tried and rejected after user listening: tank taps reading only the circulated signal and a fixed pair 2 FIR table. Dumped the ES5510 GPRs/instructions and impulse responses from the driver trace. Touched: runtime/audio/hle/effects.hpp, raw/tests/2026-10-16-kirameki-187-clap-dynamics.md, log.md.

## [2026-10-16] verify | Kirameki 0x187 voice richness, layering, and filter parity
Verified 100.0% voice and layer start parity between reference 68000 native sound driver register trace and HLE over Kirameki 0x187 (821 matched voices, 0 missing, 0 dropped). Confirmed exact volume bit parity (LVOL/RVOL diff = 0), pitch/detune parity, and step-by-step filter cutoff envelope tracking. Verified ES5505 4-pole filter SIMD kernel (`voice_kernel.cpp`) across all 4 modes (0: 2xLP+2xHP, 1: 3xLP+1xHP, 2: 2xLP+2xLP, 3: 3xLP+1xLP) against `es5505.cpp` (relative error <= 0.0028). Analyzed trumpet/brass passages (Track 2, Program 66) vs PCB: measured harmonic spectrum showing PCB 3rd harmonic prominence (+4 dB above fundamental) resulting from analog/ESP interaction. Pinned behavior in unit tests `Audio.VoiceBlockFilterModesMatchES5505Hardware` and `Audio.DirectVoiceFilterIndexMatchesDriverMath`. Touched: runtime/tests/audio.cpp, docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md, docs/notes/log.md.

## [2026-10-16] ingest | Kirameki PCB recordings vs reference/HLE
Updated the docs/notes/raw/docs/taito-f3 submodule to 9927deb (5505-5510.txt now leaves the final L/R order unconfirmed, probably OTIS/ESP order). Saved raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md (+ metrics script) comparing two PCB recordings (Kirameki sound test 0x187 and sound code 1) with the MAME-derived emulation and HLE: removing MAME's pair-0 bypass and 0.5 route matches PCB level within 0.2 dB; L/R exchange kept (recording-based); HLE reverb decay fitted to the PCB tails for Kirameki preset 5; the MAME-derived emulation's tail remains 5.3 dB low. Corrected hardware/sound.md, comparisons/hardware-vs-mame-audio-routing.md, hardware/es5510-esp.md, index.md. Touched: raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md, raw/tests/2026-10-16-kirameki-187-pcb-metrics.py.txt, hardware/sound.md, hardware/es5510-esp.md, comparisons/hardware-vs-mame-audio-routing.md, index.md, log.md.

## [2026-10-10] confirm | OTIS/ESP hardware channel routing
Ingested 12's new remark verbatim in raw/docs/otis-esp-routing-12-remark-2026-10-10.txt and recorded the user's explicit hardware confirmation. Replaced the open routing question with confirmed channel wiring in hardware/sound.md and comparisons/hardware-vs-mame-audio-routing.md; recorded the refuted reversed-numbering and mix-break claims, pin-label contradiction, upstream eaa5a0a note absent from the a070cbd submodule, second index-swap remark, and in-progress Enhanced backend implication. Updated index statuses/summaries. Touched: raw/docs/otis-esp-routing-12-remark-2026-10-10.txt, hardware/sound.md, comparisons/hardware-vs-mame-audio-routing.md, index.md, log.md.

## [2026-10-10] ingest | OTIS/ESP routing runtime experiments
Saved raw/tests/2026-10-10-otis-esp-routing-experiments.md with the commandw and landmakrj ROM hashes from SCHEMA; added test-backed runtime results and remaining physical-routing hypothesis to hardware/sound.md and comparisons/hardware-vs-mame-audio-routing.md; updated index.md. Touched: raw/tests/2026-10-10-otis-esp-routing-experiments.md, hardware/sound.md, comparisons/hardware-vs-mame-audio-routing.md, index.md, log.md.

## [2026-10-09] correction | audio-routing patch attribution
Corrected comparisons/hardware-vs-mame-audio-routing.md: attributed the “swap 1 and 3” patch to the later pin-map report and removed the unsupported attribution of game ESP SER3 programming to that report. Checked hardware/sound.md; it contains neither attribution error. Touched: comparisons/hardware-vs-mame-audio-routing.md, log.md.

## [2026-10-09] ingest | OTIS/ESP routing follow-up and chronology correction
Saved raw/docs/otis-esp-dac-routing-2026-10-09-followup.txt verbatim as 12's earlier speculation; updated hardware/sound.md to retain it as superseded history and mark only OTIS SER3 → ESP serio1 as confirmed per the user's vouching; revised comparisons/hardware-vs-mame-audio-routing.md with conditional pump/register mapping, experimental patch interpretation, and open questions; updated index.md. Touched: raw/docs/otis-esp-dac-routing-2026-10-09-followup.txt, hardware/sound.md, comparisons/hardware-vs-mame-audio-routing.md, index.md, log.md.

## [2026-10-09] ingest | relayed OTIS/ESP/DAC routing observation
Saved raw/docs/otis-esp-dac-routing-2026-10-09.txt verbatim; added the pin-map hypothesis and explicit source inconsistencies to hardware/sound.md; created comparisons/hardware-vs-mame-audio-routing.md with current MAME add_route lines; updated index.md. All claims remain hypothesis pending user confirmation. Touched: raw/docs/otis-esp-dac-routing-2026-10-09.txt, hardware/sound.md, comparisons/hardware-vs-mame-audio-routing.md, index.md, log.md.

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

## [2026-10-10] update | Enhanced OTIS→ESP hardware routing
Updated comparisons/hardware-vs-mame-audio-routing.md and hardware/sound.md: Enhanced now
uses the hardware pair routing, while Reference remains MAME-routed. Added the user-reported
Enhanced routing test and 2400-frame Land Maker RMS/peak measurement as
raw/tests/2026-10-10-enhanced-effects-hardware-routing.md; refreshed index summaries.
Also replaced the stale in-progress status in hardware/sound.md with the completed pair map.
Touched: docs/developer/HLE-AUDIO.md, runtime/audio/hle/effects.hpp,
comparisons/hardware-vs-mame-audio-routing.md, hardware/sound.md,
raw/tests/2026-10-10-enhanced-effects-hardware-routing.md, index.md, log.md.

## [2026-10-10] ingest | taito-f3 submodule bump a070cbd → eaa5a0a
Moved raw/docs/taito-f3 from a070cbd to eaa5a0a (“note on 5505-5510 channels”). The only change in
the range is the added raw/docs/taito-f3/5505-5510.txt (22 lines): 12's notes on the OTIS/ESP sound
hardware. Not ingested; no page cites it yet, and the bump changes no existing file, so no existing
citations need re-checking. Updated the provenance sidecar raw/docs/taito-f3.md: git_commit →
eaa5a0ab624464f5ae904a6227414d09d75f64ee, the git hash in captured_with → eaa5a0a, ingested →
2026-10-10. The submodule pointer is staged in the superproject and not committed.
Touched: raw/docs/taito-f3.md, log.md.

## [2026-10-10] add | landmakrj 2P versus mode transition and trigger analysis
Identified 2P versus mode transitions in Land Maker (Japan): word 0x401f6e (a5-$6092) acts as the
canonical versus latch (0 in attract and 1P, 1 in 2P versus). Tested 4 scenarios (attract only,
1P vs CPU, fresh 2P versus, and mid-game challenger). Added raw tests and disasm, routine page
for sub_08edde (versus mode transition), and updated game index.
Touched: docs/notes/raw/disasm/landmakrj-0008edde-versus-mode-transition.md,
docs/notes/raw/tests/landmakrj-2p-versus-trigger-runs.md,
docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md,
docs/notes/games/landmakrj.md, docs/notes/index.md, docs/notes/log.md.

## [2026-10-10] add | landmakrj minimal sync list (78 B) and per-frame RNG reseed resolution
Completed empirical differential sync experiment for Land Maker netplay: proved that RAM-only
minimal sync without reseed suffers from idle-loop RNG phase drift at ROM $19f8, causing stage theme
and piece generator divergence; developed Spec 5 (per-frame RNG reseed at frame boundary via
splitmix32) which erases idle-loop drift before cooperative tasks run. Shrunk sync set in Spec 6C to
exactly 78 bytes (mode block 0x401f40..0x401f83 [68B], P1 piece RNG [4B], P2 piece RNG [4B], settings [2B]),
achieving 100% byte-exact parity across all 8,000 frames of a full match. Proposed gameplay desync hash
region 0x401f40..0x402130 (496 bytes). Kept full host snapshot transfer (3.47 MB) as reference oracle.
Touched: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md,
docs/notes/raw/tests/landmakrj-2p-versus-trigger-runs.md,
docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md,
docs/notes/games/landmakrj.md, docs/notes/log.md.

## [2026-10-11] update | landmakrj lobby hold and menu un-commit architecture
Investigated lobby hold and un-commit mechanisms across all three trigger paths: Game Select menu, Charsel
Join (~1450), and Challenger (~2300). Traced task scheduler: Task 12 (sub_09e962), Task 7 (sub_003d5e sparkle engine),
Task 15 (sub_010170 raster/palette), and Task 1 (sub_08e20c). Discovered background swirl is driven by sub_09ecb0
lines-RAM sine scroll at 0x62a000 with phase 0x40790a, and countdown timer is 0x407908 (1,200 frames / 10s).
Implemented lobby_hold_frame pin rules (freezes flash counter 0x407903 at 30 and timer 0x407908 at 1,200), verified
holding 600 frames past trigger with swirl, sparkles, and BGM actively animating. Implemented uncommit hook
(clears confirm flag 0x407902, flash counter 0x407903, resets timer 0x407908 to 1,200, clears 0x401f66/6e),
verified returning cleanly to interactive Game Select with full timer and active cursor. Verified pre-commit
detection predicate (0x407902 == 1 && 0x401f66 == 0xffff).
Touched: docs/notes/raw/tests/landmakrj-differential-sync-experiment.md,
docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md, docs/notes/log.md.

## [2026-10-11] add | landmakrj post-sync lobby frames desync (session ff46d13f, frame 1740) and flash counter pin
Real session 1740 CRC mismatch reproduced as unequal numbers of pinned lobby frames run after capture_sync/apply_sync
(frontend in_lobby stays true until Running); no desync with zero tail across hold 0..3000 frames, different pre-trigger
paths and 8000-frame mashed matches. Flash counter 0x407903: overlay drawn when <= $20 and (c & 4) == 0; pin 0x30 for a
calm glow, verified with screenshots, play locally and cancel.
Touched: docs/notes/raw/tests/landmakrj-lobby-tail-desync-and-flash.md, docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md, docs/notes/log.md.

## [2026-10-11] add | landmakrj desync 695b9e18 at frame 540: unsynced player-struct residue; sync full 496 B block
Bundles replay to their own hashes only with the frontend machine config (GPU + enhanced audio); f3rt-tool netplay-replay
rejects them (state size 1701710 vs 3468025). First divergence at the gate flip (match frame 522): 0x40206c, 0x402078,
0x4020f6. Copying the 496 B block host->joiner removes it on the bundles and in a 6000-frame matrix (reproduced 0x402078
desync at frame 345 without it).
Touched: docs/notes/raw/tests/landmakrj-desync-695b-frame540.md, docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md, docs/notes/log.md.

## [2026-10-11] update | landmakrj RNG reseed keyed on round index; 0x400826 consumer audit
Audit of `0x400826` (xref plus a 2P 18,000-frame watch run): readers are `sub_001468` (LCG; idle loop `$19f8` via `sub_001174` advances it, the drift source), `sub_093462` @0x0935d4 (copies the word into BOTH players' bag seeds, once per round, ~12 frames after the stage pick), `round_task` @0x08e470 (stage step `((rng&3)+1+prev)&7` into 0x401f54), `sub_08e84c` (sound pick), `sub_0915d2` (tile/palette), `sub_0a04d0` (dice roll `rng&0x7f + table[0x4000d0][0x401f54] + 0x401f7a`, script opcode 2; role unconfirmed, Haiku read as CPU AI), and a ~40-entry `dc.l $1468` callback table (unresolved). `sub_093a1c` swaps the word with the player seed and restores it, so after init it does not consume the global. Wins sum `0x401f6a+0x401f6c` is 0/1/2 at the stage picks at frames 2631/4983/11043 and is updated 2 frames before each pick. `on_match_frame` now keys on `seed ^ (wins_sum * prime)` instead of `match_frame`: a pure function of the server seed and synced RAM, rollback-safe. Side effect: the word is constant within a round, so the `sub_0a04d0` roll and cosmetic readers repeat per round. Verified: tests/test_netplay.py 6 passed, 16,000-frame two-peer match equal sync_hash, no DESYNC. Not observed: a round-end inside that run, and rounds that end without a winner.
Touched: games/landmakrj/netplay/netplay.cpp, docs/site/developer/netplay/index.md, docs/notes/log.md.

## [2026-10-11] ingest | runtime documentation migrated into the wiki (sound hardware model, ROM layout, video producers, boot/EEPROM, MAME parity)
docs/site and docs/developer are being deleted, so their reverse-engineering content was moved here. Provenance is stated on every
page: most of it is MAME-derived runtime modelling, ROM static analysis and runtime test records, not board observation, and nothing
overrides 12's board observations (OTIS to ESP routing stays confirmed on hardware/sound). New pages are `hypothesis`. Own checks
against the supplied ROM files (stored as raw/tests records with the exact script and output): main image sha256 matches SCHEMA; FF
fills are exact (`[0x702f,0x10000)`, `[0x11b362,0x1ffffe)`, sound `[0xc1e45a,0xc20000)`, `[0xc39a30,0xc80000)`); main word `0x1ffffe` =
`0x0001`; sound image CRC32 `5a7e9117`; sound length table `0xc13260` and handler table `0xc1323e` match the documented commands; 27
sequences in both directories (header stride 0xa8); main selector table `0x6bde` has 0x60 entries. Dropped or downgraded: the claim
that Land Maker "sets output gain at ~780 frames" is only arithmetic (13.2338 s x 58.94 Hz = 780.05; `boot.cpp` measures boot completion at
frames 781/478, not gain); the 27 sequences / 206 tracks / 24,470 events and 100 programs (79 layered) come from deleted
`tools/hle_sequence.py` / `hle_instruments.py` and only the 27 was reproduced. Not migrated: binary-size, GPU-performance and
optimization measurements, ABI history, recompiler design rationale. SCHEMA gained tags `rom-layout, eeprom, netplay, sound-driver,
mailbox`, a provenance note for runtime-derived docs, and its bus-map pointer now targets hardware/memory-map.
Touched: docs/notes/SCHEMA.md, docs/notes/index.md, docs/notes/log.md, docs/notes/games/landmakrj.md (new `## ROM layout`, boot/EEPROM,
routine index), docs/notes/games/commandw.md, docs/notes/hardware/sound.md, docs/notes/hardware/fda.md (palette precision section),
docs/notes/hardware/es5505-otis.md, docs/notes/hardware/es5510-esp.md, docs/notes/hardware/duart-and-volume.md,
docs/notes/hardware/sound-mailbox.md, docs/notes/routines/landmakrj-000041d0-sprite-list-writers.md,
docs/notes/routines/landmakrj-000a8e0c-display-object-loop.md, docs/notes/routines/landmakrj-00005cd8-line-ram-producers.md,
docs/notes/routines/commandw-00009a0e-object-loop.md, docs/notes/routines/landmakrj-000030be-sound-selector-dispatch.md,
docs/notes/routines/landmakrj-00c12ecc-sound-command-dispatch.md, docs/notes/routines/landmakrj-00c10da0-sound-timer-irq.md,
docs/notes/routines/landmakrj-00c14730-sequence-event-decode.md, docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md
(shipped v4 hook, round-keyed reseed), docs/notes/comparisons/runtime-vs-mame-attract-capture.md, and new raw files: raw/docs/runtime-audio-{es5505,es5510,duart-and-gain,mailbox,sequences}.md,
raw/docs/runtime-hle-audio-rom-protocol.md, raw/docs/runtime-game-config-reference.md, raw/docs/runtime-video-producers.md,
raw/docs/runtime-sprite-units-commandw.md, raw/docs/runtime-eeprom-and-fast-boot.md, raw/docs/runtime-decisions-rom-set-identity.md,
raw/docs/landmakrj-config-{excerpts,entry-points,lanes}.toml.txt, raw/emu-source/{landmakrj-video,landmakrj-boot,landmakrj-netplay}.cpp.txt,
raw/emu-source/commandw-sprite-behaviours.hpp.txt, raw/emu-source/runtime-fdp-palette15-blur.cpp.txt, raw/tests/landmakrj-{ff-fill-extents,
rom-exclusion-profile,mame-attract-capture-parity,seed5-sound-trace,sound-rom-structure-check,sound-image-crc,main-sound-selector-table-check,
video-survey-fallbacks,sprite-census,rng-reseed-audit}.md, raw/tests/landmakr-vs-landmakrj-config-diff.md.

## [2026-10-12] ingest | sound driver reverse-engineering facts migrated from retired documentation into wiki backed by ROM disassembly excerpts
Migrated all facts from retired sound driver reverse-engineering documentation not yet in the wiki into docs/notes/ backed by static M68K Capstone disassembly of the interleaved sound ROM (CRC32 5a7e9117) and main ROM (sha256 aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc):
- Reset vector: offset 4 longword $00c1089e verified on sound image and documented on games/landmakrj.md.
- Main selector dispatch: type 0 helper main $30ee emits 81 then 86; type 2 helper main $31cc emits optional old-note 8f, then 8d and 8e; music selector descriptors IDs $4e..$5d confirmed in table $6bde (type 0 descriptors $6fd0..$6ffd) with max selector $5f at $6d5e on routines/landmakrj-000030be-sound-selector-dispatch.md.
- Command dispatch: opcode 82 handler $c12f1a branches to $c12c30 to stop/remove sequence; 8d handler $c130da branches to $c11dce; 8c handler $c130d2 doubles index and dispatches controller via $c14330; 8e handler $c130ea writes key at $c130f0, duration $7fff, branches to note factory $c140d6; sequence bit 7 extra selection/state at $c12c30/$c12cb8/$c12cea; linked sequence alias chain walk $c12de6 checking header byte +$a6 via $c158cc on routines/landmakrj-00c12ecc-sound-command-dispatch.md.
- Table access & note ownership: sequence slot address calc $c12a46 ($5e5c stride $28); active sequence list insertion $c12dca ($6dfc); stream bank long offset $c158ae ($d404); header bank word offset $c158cc ($d408); channel lookup $c11c1a ($5daa); channel sequence link $c159a0 ($e(a1) = $5e5c + seq*$28); voice record init $c17a00..$c17a46 ($d94a stride $aa); voice initializer $c17b94 (copies note node to voice +$0e, channel pointer to +$10); split table selection $c17ce6 ($c1c1e0 relative to $c1ae76); fixed ROM tables ($c089dc signed multipliers, $c08bec voice scaling, $c0a734 loop-mode to OTIS control byte) on routines/landmakrj-00c14730-sequence-event-decode.md.
- Voice allocation: created new page routines/landmakrj-00c1748e-voice-allocation.md covering $c1748e search, $c1753a..$c17664 legato/retrigger reuse without key-on (owner change at $c1762c visible at $c17632 write), $c17670/$c17678 exhaustion scan of $d850/$d848 by priority class (patch +$60 bits 6-7 saved in voice +$97), $c176f8 carry rejection, $c17b94 voice init, $c17e62 OTIS setup, $c177f6 pending walk, $c17814 CR stop clear unless bit 4 suppresses, $c1781c rotating links and pending-to-active splice, and $c1789a unlinking to $d844 and silencing via $c17a88.
- DUART: initialization routine $c10e22 programs counter preset $07d0 at $c10e66, ACR $60 at $c10e6e, IMR $2b at $c10e76 on hardware/duart-and-volume.md.
- DSP worker queue & effects: queue $fb28 posted by $c0f430; contradiction resolved: $c0f482 is the arithmetic helper calculating $c1c2d2 + 16*d0 while $c0f4bc is the worker routine applying the configuration and loading the DSP program on hardware/es5510-esp.md.
- References to retired driver document rewritten across non-raw notes pages (games/landmakrj.md, hardware/sound-mailbox.md, routines/landmakrj-000030be-sound-selector-dispatch.md, routines/landmakrj-00c12ecc-sound-command-dispatch.md).
- Retired driver documentation docs/SOUND-DRIVER.md deleted from repository root via git rm.
Touched: docs/notes/raw/disasm/landmakrj-sound-driver-excerpts.md, docs/notes/games/landmakrj.md, docs/notes/hardware/sound-mailbox.md, docs/notes/routines/landmakrj-000030be-sound-selector-dispatch.md, docs/notes/routines/landmakrj-00c12ecc-sound-command-dispatch.md, docs/notes/routines/landmakrj-00c14730-sequence-event-decode.md, docs/notes/routines/landmakrj-00c1748e-voice-allocation.md, docs/notes/hardware/duart-and-volume.md, docs/notes/hardware/es5510-esp.md, docs/notes/index.md, docs/notes/log.md, docs/SOUND-DRIVER.md.

## [2026-10-15] ingest | board RAM layout and piece queue/generator probe (landmakrj)
Two headless 1P-versus-CPU runs (build/f3a/board-probe-a: 6000 frames, dumps every 50 from 1000; board-probe-b: 2950 frames, dumps every 2 from 1750) with the mash script coin-start-mash.txt, plus static disassembly of sub_09c854, sub_09c244/sub_09c260, sub_09c474, sub_0938f2, sub_093462 and sub_093a1c. Recorded: board bases P1 0x4073b6 / P2 0x407676 (launcher rows 0x407376 / 0x407636), cell address = base + 0x20*R + 4*C, honeycomb playable cells, wall init, cell encoding `FF TT PPPP`; piece queue fields +0x90/+0x92/+0x50/+0x98, colour masks 0x40715c/0x40715e, queue shift and the colour generator (rejection sampling over colours on the player's own board). The probe's byte0 = 0xff row timeline was re-read from the dumps: none at frame 2950, rows 15-16 at 3000..3200, rows 13-16 at 3250..3650 (P1 only). Open: byte0 0xff writer and meaning, colour names, the 8x/9x player split and specials (needs a 2P versus run), fixed stage-0xffff/0xfffe sequences.
Touched: docs/notes/raw/tests/landmakrj-board-and-piece-queue-probe.md (new), docs/notes/routines/landmakrj-0009c854-board-cells.md (new), docs/notes/routines/landmakrj-00093a1c-piece-generator.md (new), docs/notes/games/landmakrj.md (new `## Gameplay RAM (board and piece queue)`, routine index, frontmatter), docs/notes/index.md, docs/notes/log.md.

## [2026-10-15] ingest | taito-f3 submodule bump a070cbd → d95fddf (5505-5510 audio interconnect, clocks, FCM glue logic, analog audio)
Updated git submodule raw/docs/taito-f3 to upstream HEAD (commit d95fddf8660f318578fff0f6fd4ae8d0108feda1) and ingested changes since a070cbd across commits eaa5a0a, fea9d1d, be2f796, d95fddf:
- 5505-5510.txt (new): 12's oscilloscope signal tracing and timing measurements for the serial audio interconnect between OTIS (ES5505), FCM (TC0660FCM), ESP (ES5510), and TDA1543 DAC. Documents physical channel routing through FCM (OTIS ser1..3 → FCM → ESP serio0..2, ESP serio3 → FCM → DAC data), register presets (OTIS Host Serial Control = $63 Sony slave mode, ESP SERMODE = $07F8 normal), FCM replacement of a 74x174 4x D flip-flop (clocked by OTIS BCLK) and 74x373 8x transparent latch (clocked by ESP GATE), and 128-BCLK LRCLK frame timing (16 bits high left sample, 112 bits low right sample, BCLK rising edge data transition, inverted DAC LRCLK phase).
- clocks.txt (new): master clock frequencies (main CPU 15.23809 MHz, audio CPU 15.23809 MHz, OTIS 15.23809 MHz, ESP 8 MHz, DUART 4 MHz) and audio serial bit clock (BCLK = 3.8095225 MHz, 15.23809/4) and sample rate (~29.76189453 kHz, BCLK/128).
- chips.txt: added MC33274 [AP] quad op-amp, TDA1543 zig-zag pin counting note, TA8221A 30W dual audio power amp, and TC0660FCM link to Ensoniq 5701 "SUPER GLU" datasheet.
- list.txt: audio clock nets (BCLK, WCLK, LRCLK); DAC analog output chain (dac.6/8 into op-amps ic53, ic54, ic63 to stereo output); ESP control/bus lines (esp halt, esp cs/as, esp/otis rw, esp gate, esp hdb0..7).
- website/fcm.html: updated FCM pinout table with audio serial pins (48-49, 51-58) and ESP RAM address/data latch pins (62-79).
Removed old caveats stating 5505-5510.txt was absent from the checkout. Updated wiki pages with ^[raw/docs/taito-f3/5505-5510.txt] citations and refreshed index.md.
Touched: docs/notes/raw/docs/taito-f3 (submodule commit d95fddf), docs/notes/raw/docs/taito-f3.md, docs/notes/hardware/sound.md, docs/notes/comparisons/hardware-vs-mame-audio-routing.md, docs/notes/hardware/es5505-otis.md, docs/notes/hardware/es5510-esp.md, docs/notes/hardware/clocks.md, docs/notes/hardware/board.md, docs/notes/hardware/fcm.md, docs/notes/index.md, docs/notes/log.md.

## [2026-10-15] update | reconcile OTIS->ESP->DAC audio routing and SingleStage default gain model across wiki
Reconciled internal contradictions left behind in wiki notes following taito-f3 submodule bump to d95fddf (which added 5505-5510.txt):
- Replaced superseded 2026-10-09 1<->3 index swap claims across hardware/sound.md and comparisons/hardware-vs-mame-audio-routing.md with confirmed oscilloscope-grounded routing from 5505-5510.txt: OTIS pin 6 (ser0, Pair 0) -> NC (unconnected); OTIS pin 7 (ser1, Pair 1) -> FCM -> ESP pin 15 (serio0, channel 0, Dry); OTIS pin 8 (ser2, Pair 2) -> FCM -> ESP pin 14 (serio1, channel 1, Delay); OTIS pin 9 (ser3, Pair 3) -> FCM -> ESP pin 13 (serio2, channel 2, Reverb); ESP pin 12 (serio3, channel 3) -> FCM -> TDA1543 DAC pin 3 (data).
- Documented that the earlier 2026-10-09 relay (otis-esp-dac-routing-2026-10-09.txt) numbered ESP pins 12–15 in reverse order (serio3..serio0) before 5505-5510.txt corrected channel labels against the ES5510 datasheet, creating an apparent 1<->3 swap that is refuted by the physical wiring.
- Clarified that MAME (taito_en.cpp) actually routed OTIS Pairs 1, 2, 3 to the correct ESP channels (1->0, 2->1, 3->2), and that its real audio routing bugs were routing unconnected OTIS Pair 0 to the output mixer as an Aux bypass, applying MB87078 volume twice (before ESP and after pump), and omitting the L/R channel swap caused by Sony slave LRCLK polarity at the TDA1543 DAC.
- Grounded runtime HLE audio (runtime/audio/hle/effects.hpp) mapping Pair 1 (buses 2/3) -> Dry, Pair 2 (buses 4/5) -> Delay, Pair 3 (buses 6/7) -> Reverb, Pair 0 (buses 0/1) -> NC, and updated hardware/sound.md, hardware/duart-and-volume.md, hardware/es5510-esp.md, and comparisons/hardware-vs-mame-audio-routing.md to reflect that SingleStage (single-stage post-DAC analog attenuation, matching raw/docs/taito-f3/list.txt:265-280) is now the runtime's default gain model.
Touched: docs/notes/hardware/sound.md, docs/notes/comparisons/hardware-vs-mame-audio-routing.md, docs/notes/hardware/es5510-esp.md, docs/notes/hardware/duart-and-volume.md, docs/notes/index.md, docs/notes/log.md.

## [2026-10-16] kirameki 0x187 HLE direct-voice envelope fix
- HLE direct voices now dispatch envelope-timer expiry by stage like the driver (c1971c/c1983c): expiry starts the release ramp instead of holding forever (unreleased) or stopping at full level (note-off ignored by patch byte0 bit7). Evidence and measurements: docs/notes/raw/tests/2026-10-16-kirameki-187-direct-voice-envelope.md. The HLE declick tail (commit 480969a: TAIL_DECAY, cut_output, mix_tail, per-voice last-output state, landmakrj test check 6) was removed: with the fix it has no measurable effect and HLE without it matches the reference backend's cut steps.

## [2026-10-16] kirameki 0x187 HLE reverb stereo topology and return gain fix (ES5510 DSP microcode)
- Disassembled the 89-instruction ES5510 DSP microcode program for preset 5 from Kirameki sound ROM. Revealed two discrepancies in HLE `effects.hpp`:
  1. Reverb wet return gain: in the ES5510 microcode (instructions 01 and 03), the delay/reverb output is scaled by `G125 = table(127 - p8) = 0.9227` (preset 5 p8 = 32), while HLE previously set `m_wet_reverb = table(p9) = 0.4639` (-6 dB deficit). Corrected `m_wet_reverb = table(127 - m_parameters[8])` and `m_dry_reverb = table(127 - m_parameters[9])`.
  2. Reverb input cross-feed and stereo tap decorrelation: in the ES5510 microcode, the reverb tank is cross-fed (`G86 = SER2L + table(p9) * SER2R` at instruction 52) and the outputs to SER3L and SER3R read from different circulating delay taps ([2461, 5159, 7470] vs [2699, 5313, 8191]). Grounded in this microcode, HLE now applies `m_reverb_cross = table(m_parameters[9])` normalized cross-feed in `render_delay_block` and decorrelating cross-mix between left and right diffuser outputs.
  3. Re-calibrated `K_DECAY_PER_P0 = .91f / 40.f` against both PCB recordings (A: tail error 2.31 dB, bias +0.99 dB, LRcorr 0.787 vs PCB 0.699; B: tail error 1.70 dB, bias -0.56 dB, LRcorr 0.709 vs PCB 0.539; max 8-14.7 kHz error 0.96 dB). In the 2.326-2.713 s clap window, tail side level improved from -37.7 dB to -27.9 dB (+10 dB stereo width) and post-clap L-R correlation dropped from 0.95 to 0.61-0.76 towards the PCB.
- Tests: `Audio.ReverbWetReturnAndCrossFeedMatchDspMicrocode` added to `runtime/tests/audio.cpp`.
Touched: runtime/audio/hle/effects.hpp, runtime/tests/audio.cpp, docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md, docs/notes/raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md, docs/notes/log.md.

## [2026-10-16] kirameki 0x187 HLE note timing: driver busy clock and start queue
- HLE started every voice of a 1 kHz sequencer pulse on the same tick; the real driver programs the voices one after another (cycle-accurate driver trace: 17.8k-20.4k sound-CPU cycles per voice, 1.1-1.3 ms), spreading a 19-voice downbeat over about 24 ms. New runtime/audio/hle/note_timing.hpp (`DriverClock`, `NoteQueue`), `DRIVER_KEYON_CYCLES = 20408` in events.hpp, scheduling in synth.cpp (`launch`, `service_queue`, `note_on`/`note_off`/`reset`, render event loop): start = max(tick, busy), busy += cost per layer started, queued starts land on their exact output frame, a note-off that arrives before its queued start is applied after it with the original note length (no note dropped; 1523 note-ons before and after).
- PCB/driver check on 0x187: key-on delta vs the real driver std 7.29 -> 2.86 ms, envelope correlation with the PCB 0.281 -> 0.416 (reference 0.572); PCB optimum plateau 24k-28k cycles is reported, driver-derived 20408 kept (docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md section 9).
- Tests: HleNoteTiming.* (runtime/tests/audio.cpp), HleAudio.KiramekiPulseNotesStartOneDriverSlotApart.
Touched: runtime/audio/hle/note_timing.hpp, runtime/audio/hle/events.hpp, runtime/audio/hle/synth.cpp, runtime/tests/audio.cpp, games/kirameki/tests/audio.cpp, docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md.

## [2026-10-16] kirameki 0x187 replay: plain 0x81 starts finish at the bar boundary
- HLE restarted sequence 0x2c from its start after its last note (reference: last start 20.214 s; HLE replay from 19.8 s). The driver re-enters a sequence at its bar boundary only through an arrangement link; `pulse()` restarted every plain start. Fix: `runtime/audio/hle/sequencer.cpp` finishes a sequence with no arrangement owner at its bar boundary, and removes the now-dead `looping` flag. Kirameki 0x187: 1025 starts before 19.8 s unchanged, 1523 -> 1055 starts total, last start 20.142 s (reference 20.214 s). Landmakrj song 10 stops at 33.8 s (was 36.4 s replay); landmakrj selection loop 0x87 unchanged (3671 starts).
Touched: runtime/audio/hle/sequencer.cpp, games/kirameki/tests/audio.cpp, docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md (section 13).

## [2026-10-16] landmakrj sound-extract: request routine $00308c, SOUND NO = selector code
- Land Maker's SOUND TEST SOUND NO item (1..word `$6d5e` = 0x5f, shown as 3 hex digits) calls `$00308c(code)` directly, with no category or prelude step; effect preset/parameters are type 3 selectors `$5e`/`$5f` sent at boot. `f3rt-tool sound-extract --sound 0x5C` (song `$19`, vol `$76`) now works through the game's own routine; `--bank` on a set without sound bank switching is an error.
Touched: tools/commands/sound_extract.cpp, docs/notes/raw/disasm/landmakrj-sound-selectors.md.
