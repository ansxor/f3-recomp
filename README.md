# f3-recomp

Taito F3 static recompilation + modern runtime, modeled on N64Recomp + N64ModernRuntime.
Target: Land Maker (`landmakr`, main CPU 68EC020) running natively and matching MAME's behavior.

## split
- `recomp/`   tool: ROM -> C (68020 lifter, function discovery, jump-table/indirect handling, per-game TOML config). Output C calls only the runtime ABI.
- `runtime/`  library `f3rt`: memory map + I/O, interrupts/vblank timing, FDP (video), OTIS/ES5505 + sound CPU (audio), input, EEPROM, SDL3 frontend.
- `include/f3rt/` the ABI between the two. Owned by runtime; recomp consumes. Change it only with a note in docs/ABI-CHANGES.md and tell the other session.
- `games/landmakr/` per-game config + generated C (generated C and ROM data are gitignored).
- `tools/mame/`  MAME lua scripts that dump traces (memory writes, regs, frames, audio) for differential testing.

ROMs live outside the repo: `../roms/<set>/`.
