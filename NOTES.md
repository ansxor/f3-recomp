# Runtime decisions

- 2026-10-03: `f3rt` is a standalone C++20 library with a C CPU ABI, SDL3 frontend, software FDP renderer, and permissively licensed Musashi CPU interpreter for independent execution. No MAME framework dependency at runtime.
- Reference is local MAME `taito_f3.cpp` / `taito_f3_v.cpp`, not inferred physical mirrors where MAME disagrees. Main CPU is 16 MHz per MAME; raster is 26,686,000 / 4 Hz, 432 x 262 total, 320 x 232 visible (x=46,y=24). Vblank IRQ2 and delayed IRQ3 (+10,000 main cycles) match MAME. MAME does not implement timer IRQ5; do not introduce physical timer behavior that changes the reference path.
- CPU ABI being frozen with recomp peer: big-endian accessors, explicit CPU context, per-block interrupt boundary, exception entry, generated block dispatch, and interpreter fallback. Runtime owns the published header.
- ROMs, generated C, captures, and decoded assets remain untracked. Raw ROM contents must never be committed.
- Video and audio adaptations must retain upstream attribution and licensing. Renderer correctness is judged against captured MAME frames; audio is checked with executable device/ROM scenarios, not a no-op audio path.
