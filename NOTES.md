# Runtime decisions

- 2026-10-03: `f3rt` is a standalone C++20 library with a C CPU ABI, SDL3 frontend, software FDP renderer, and permissively licensed Musashi CPU interpreter for independent execution. No MAME framework dependency at runtime.
- Reference is local MAME `taito_f3.cpp` / `taito_f3_v.cpp`, not inferred physical mirrors where MAME disagrees. Main CPU is 16 MHz per MAME; raster is 26,686,000 / 4 Hz, 432 x 262 total, 320 x 232 visible (x=46,y=24). Vblank IRQ2 and delayed IRQ3 (+10,000 main cycles) match MAME. MAME does not implement timer IRQ5; do not introduce physical timer behavior that changes the reference path.
- CPU ABI being frozen with recomp peer: big-endian accessors, explicit CPU context, per-block interrupt boundary, exception entry, generated block dispatch, and interpreter fallback. Runtime owns the published header.
- ROMs, generated C, captures, and decoded assets remain untracked. Raw ROM contents must never be committed.
- Video and audio adaptations must retain upstream attribution and licensing. Renderer correctness is judged against captured MAME frames; audio is checked with executable device/ROM scenarios, not a no-op audio path.

- The supplied `roms/landmakr` is `landmakrj` 2.01J. Orchestrator changed the target accordingly; world `landmakr` loading remains configured but untested. The two short sound program chips become exact current-MAME CRC/SHA1 dumps when their missing upper halves are filled with FF. Four required board PLDs come from supplied Puchi Car dumps with exact matching CRCs; no substitute data is fabricated.
- ABI v1 frozen with peer; initial crossed drafts resolved in `docs/ABI-CHANGES.md`.
- Built standalone runtime and linked generated C. Behavioral smoke passed memory mirrors/byte lanes, input and coin edges, EEPROM write protection and sequential wrap, 68020 IRQ/stack entry, native dispatch, and one-instruction fallback.
- SDL3 actual window run: `video_driver=cocoa renderer=metal`, 120 frames, renderer readback saved under ignored `build/window.bmp`.
- Independent MAME capture works using clean shared baseline. Capture taps require strong Lua references and reset reinstallation; otherwise scroll state is silently lost. `build/captures/mame-verified` is the corrected 25-frame reference (600..3480, step 120).
- Video parity work remains active: corrected doubled RAM offsets and host-word character decoding. Replay now exactly matches 15/25 sampled frames; remaining animated/alpha mismatches are under investigation. No final parity claim.
- Audio remains under investigation: sound CPU runs but initial 3600-frame ROM smoke is silent. Corrected OTIS accumulator mask (31, not 27 bits), CPU-only RESET preservation, and sound CPU cycle-overrun accounting. These are not yet sufficient for attract sound.
- Strengthened acceptance: only an integrated generated-C executable with attract, playable coin/start/input, sound, and MAME parity qualifies as DONE. Recomp peer owns root `integration` branch merges and unified command. Interpreter/replay are validation aids only.
