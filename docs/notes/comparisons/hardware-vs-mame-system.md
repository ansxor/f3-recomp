---
title: Hardware notes vs MAME 0.289 — timing, interrupts, memory map, I/O
created: 2026-10-09
updated: 2026-10-09
type: comparison
tags: [timing, interrupt, vblank, io, input, memory-map, cpu, emulator-bug, undocumented]
sources:
  - raw/emu-source/mame-0.289/taito_f3.cpp
  - raw/emu-source/mame-0.289/taito_f3.h
  - raw/docs/taito-f3/interrupt.txt
  - raw/docs/taito-f3/website/video.html
  - raw/docs/taito-f3/mem.txt
  - raw/docs/taito-f3/address.txt
  - raw/docs/taito-f3/fio.txt
  - raw/docs/taito-f3/fio-mem.txt
games: []
addresses: [0x00400000, 0x00440000, 0x004A0000, 0x004C0000, 0x00600000, 0x00660000, 0x00C00000, 0x00C80000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/interrupt.txt, note: "12Me21 measured interrupt intervals and timer formula on hardware"}
  - {kind: doc, ref: raw/docs/taito-f3/fio.txt, note: "FIO write map and pin tracing"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "machine config, address map, interrupt and input code"}
contradictions:
  - "Interrupt 3 delay: notes measure about 712 us (about 10850 CPU cycles) after interrupt 2; MAME schedules it 10000 CPU cycles after interrupt 2 on a 16 MHz CPU (625 us)."
  - "Main CPU clock: notes use 30.47618 MHz / 2 (about 15.24 MHz); MAME uses 16 MHz and its own header comment says 'should be 30.47618_MHz_XTAL / 2'."
  - "Interrupt 5 (timer at 0x4C0000): notes document a working interval timer; MAME only logs writes and never raises interrupt 5."
  - "Palette RAM extent: notes list 0x440000-0x45FFFF; MAME maps 0x440000-0x447FFF."
  - "Coin/button fields: notes' hardware dump puts player 1/2 button 4 in word 0 of the FIO read block; MAME puts buttons 1-4 in the low word of long 0 (bits 0-7)."
supersedes: []
---

# Hardware notes vs MAME 0.289 — timing, interrupts, memory map, I/O

Companion to [[comparisons/hardware-vs-mame-video]]. Rows have both a notes citation (`doc`) and a MAME
citation (`emu-source`); neither side is confirmed. Related hardware pages: [[hardware/video-timing]],
[[hardware/interrupts]], [[hardware/fio]], [[hardware/memory-map]], [[hardware/clocks]]. Schema: [[SCHEMA]].

Result column: **agree**, **differ**, or **model** (same data, different description).

## Clocks and raster

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Pixel clock | 6.6715 MHz (26.686 MHz / 4) | `26.686_MHz_XTAL / 4` | agree | ^[raw/docs/taito-f3/website/video.html; raw/emu-source/mame-0.289/taito_f3.cpp L458-L461] |
| Raster size | 432 pixels × 262 lines, ~58.94 Hz | `set_raw(..., 432, ..., 262, ...)`, "refresh rate = 26686000/4/432/262 = 58.94 Hz" | agree | ^[raw/docs/taito-f3/website/video.html; raw/emu-source/mame-0.289/taito_f3.cpp L458-L463] |
| Visible area | Image starts at pixel 64 after hsync on lines 24-254 (320 px wide, 232 lines) | Visible x 46..365, y 24..255 | agree | ^[raw/docs/taito-f3/website/video.html; raw/emu-source/mame-0.289/taito_f3.cpp L459-L462; raw/emu-source/mame-0.289/taito_f3.h L111-L116] |
| CPU clock | 30.47618 MHz / 2 (≈15.24 MHz), 986.7 cycles per scanline | `F3_MAIN_CLK = 16_MHz_XTAL`, with a header comment that it should be `30.47618 / 2` | differ (acknowledged by MAME) | ^[raw/docs/taito-f3/interrupt.txt; raw/emu-source/mame-0.289/taito_f3.h L108-L109] |
| Bootleg raster | not covered | Separate `bubsympb` config uses 58.97 Hz and a 624 us vblank | MAME-only | ^[raw/emu-source/mame-0.289/taito_f3.cpp L543-L546] |

## Interrupts

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Interrupt 2 | Fires at the start of vblank, near the end of scanline 255 after the last visible pixel | `set_vblank_int` → level 2 `HOLD_LINE` | agree | ^[raw/docs/taito-f3/interrupt.txt; raw/emu-source/mame-0.289/taito_f3.cpp L423-L426, L448] |
| Interrupt 3 | About 712 us (~10850 CPU cycles) after interrupt 2, in the middle of vblank | Timer set to 10000 CPU cycles after interrupt 2, level 3 `HOLD_LINE`; comment "some signal from video hardware?" | differ (delay) | ^[raw/docs/taito-f3/interrupt.txt; raw/emu-source/mame-0.289/taito_f3.cpp L416-L427] |
| Interrupt 5 / timer register | `0x4C0000`, 16 bits `e.rrr rrrr rrrr` (e = enable, r = rate); fires every `0x4000 − rate·8 + 32` CPU cycles (fits measurements to ~10 cycles); games use `0x78B` (`0x278B` with enable) | Writes to `0x4C0000` are only logged; offset 2 pops a message; the TODO lists values games write at boot (`0x0000`, `0x0100`, `0x0090`, `0x278b`) and calls it "timer-based pseudo-hblank int5" | differ (not emulated) | ^[raw/docs/taito-f3/interrupt.txt; raw/emu-source/mame-0.289/taito_f3.cpp L135-L176] |
| Rate table | Measured: `0x278B` = every 64.14 us; `0x2780` = 69.88 us; `0x2700` = 137.5 us; `0x2F00`/`0x2C00` cause a reset loop | not present | notes-only | ^[raw/docs/taito-f3/interrupt.txt] |
| Interrupt 5 side effect | MAME's TODO says games configure it at POST and several write `0x0100`/`0x0000`; notes show those values never fire (`0x0100` → "no av5") | consistent: no interrupt either way | agree | ^[raw/docs/taito-f3/interrupt.txt; raw/emu-source/mame-0.289/taito_f3.cpp L137-L170] |

## Address map

| Region | Notes say | MAME 0.289 maps | Result | Source |
|---|---|---|---|---|
| Program ROM | `0x000000`-… (cart-dependent) | `0x000000-0x1fffff` ROM | agree | ^[raw/docs/taito-f3/address.txt; raw/emu-source/mame-0.289/taito_f3.cpp L183] |
| Work RAM | `0x400000-0x43FFFF` (17 address bits, so mirrors); `0x4E0000-0x4FFFFF` mirror | `0x400000-0x41ffff` RAM, mirror `0x20000` | agree (on the visible 128 KB with a mirror) | ^[raw/docs/taito-f3/mem.txt; raw/docs/taito-f3/address.txt; raw/emu-source/mame-0.289/taito_f3.cpp L185] |
| Palette RAM | `0x440000-0x45FFFF` (16-bit address) | `0x440000-0x447fff` through `palette_24bit_w` | differ (extent) | ^[raw/docs/taito-f3/mem.txt; raw/emu-source/mame-0.289/taito_f3.cpp L186] |
| `0x480000-0x49FFFF` | "returns lower byte of address" | not mapped | notes-only | ^[raw/docs/taito-f3/mem.txt] |
| FIO | `0x4A0000-0x4BFFFF` (5 address bits); writes at 0 (watchdog), 1 (coins P1/2), 4 (EEPROM), 5 (coins P3/4); address.txt gives `0x4A0000-0x4A001F`, fio.txt gives `-0x4A001B` | `0x4a0000-0x4a001f` (`0x4a001b` on the bootleg map): reads long 0-5, writes cases 0, 1, 4, 5 | agree | ^[raw/docs/taito-f3/mem.txt; raw/docs/taito-f3/fio.txt; raw/docs/taito-f3/address.txt; raw/emu-source/mame-0.289/taito_f3.cpp L62-L95, L187, L220] |
| Sprite RAM | Graphics RAM `0x600000-0x63FFFF`; sprite entries at `0x600000+`, 4 banks | `0x600000-0x60ffff` | model (MAME maps only the first 64 KB) | ^[raw/docs/taito-f3/address.txt; raw/docs/taito-f3/website/fdp/sprite.html; raw/emu-source/mame-0.289/taito_f3.cpp L189] |
| Tilemap / text / char / line RAM / pivot | Tile data from `0x610000`, pivot tiles `0x61C000`, pivot textures `0x61E000`, line RAM `0x620000`, pixel banks `0x620000`/`0x630000` | `0x610000-0x61bfff` playfield, `0x61c000` text, `0x61e000` char, `0x620000` line RAM, `0x630000` pivot | agree | ^[raw/docs/taito-f3/website/fdp/tilemap.html; raw/docs/taito-f3/website/fdp/pivot.html; raw/emu-source/mame-0.289/taito_f3.cpp L190-L194] |
| Graphics RAM mirror | `0x680000-0x6B0000` mirror of graphics RAM | not mapped | notes-only | ^[raw/docs/taito-f3/mem.txt] |
| Scroll registers | `0x660000-0x66001F` (mirror at `0x6E0000`) | `0x660000-0x66000f` and `0x660010-0x66001f`, write-only | agree | ^[raw/docs/taito-f3/mem.txt; raw/docs/taito-f3/scroll-regs.txt; raw/emu-source/mame-0.289/taito_f3.cpp L195-L196] |
| Shared (dual-port) RAM | `0xC00000-0xC7FFFF` (11 address bits) | `0xc00000-0xc007ff` | agree | ^[raw/docs/taito-f3/mem.txt; raw/emu-source/mame-0.289/taito_f3.cpp L197] |
| Sound reset | `0xC80000-0xCFFFFF`: "reads 18,FC,00,FF repeated ?"; address.txt: bit 8 of the address matters | write at `0xc80000` releases reset, `0xc80100` asserts it | model | ^[raw/docs/taito-f3/mem.txt; raw/docs/taito-f3/address.txt; raw/emu-source/mame-0.289/taito_f3.cpp L198-L199, L107-L115] |
| `0x300000` | not mentioned (cartridge bank-switch region) | `0x300000-0x30007f` bank-switch write, implemented only for Kirameki | MAME-only | ^[raw/emu-source/mame-0.289/taito_f3.cpp L117-L131, L184] |
| Unmapped regions | `0x460000`, `0x470000`, `0x500000`, `0x580000`, `0x640000`, `0x650000`, `0x660020+`, `0xD00000`+: access freezes the board | not mapped; MAME logs/ignores | notes-only | ^[raw/docs/taito-f3/mem.txt] |

## Input and output

| Feature | Notes say | MAME 0.289 does | Result | Source |
|---|---|---|---|---|
| Read block | 16 words (`fio-mem.txt`, dump from hardware): words 1/3 hold starts, buttons 1-3, services and joysticks, words 4-7 and C-F dials | Six 32-bit reads; IN.0 and IN.1 as in the notes for the low word; IN.2/IN.3 analog, IN.4/IN.5 players 3/4 | agree on buttons 1-3, starts, tilt, services, joysticks | ^[raw/docs/taito-f3/fio-mem.txt; raw/emu-source/mame-0.289/taito_f3.cpp L249-L306] |
| Button 4 (P1/P2) | Word 0 (`FEFE`/`FDFE`: bits 8 and 9 of word 0) | Low word of long 0 bits 3 and 7 | differ | ^[raw/docs/taito-f3/fio-mem.txt; raw/emu-source/mame-0.289/taito_f3.cpp L255, L259] |
| Pin-level hint | Pin list ties inputs to `i0.N` bits: P1 buttons 1-3 `i0.0-2`, tilt `i0.8`, services `i0.9-11`, starts `i0.12-15`, joysticks `i1.0-7`; "eeprom read is i0.16-31 ?" | Same bit positions; MSW of long 0 reads the EEPROM/coin input (`0x00ff0000` and `0xff000000`) | agree | ^[raw/docs/taito-f3/fio.txt; raw/emu-source/mame-0.289/taito_f3.cpp L251-L269] |
| Coin readback | `coin_r` in long 1 high word (notes' own transcription of MAME's map) | `f3_coin_r<0>`/`<1>` return the last-written coin word | agree | ^[raw/docs/taito-f3/fio.txt; raw/emu-source/mame-0.289/taito_f3.cpp L57-L60, L282] |
| Write offsets | 0 watchdog, 1 P1/2 coin, 4 EEPROM, 5 P3/4 coin | Same cases; coin bits (24..27): lockouts active-low bits 24-25, counters bits 26-27 | agree | ^[raw/docs/taito-f3/fio.txt; raw/emu-source/mame-0.289/taito_f3.cpp L71-L95] |
| EEPROM wiring | Write is `i4.0-7`; DI `i4.2`, CLK `i4.3`, CS `i4.4` (pin list) | `m_eepromout->write(data, 0xff)` on offset 4, byte 0 | model | ^[raw/docs/taito-f3/fio.txt; raw/emu-source/mame-0.289/taito_f3.cpp L88-L91] |
| Watchdog | Offset 0 write is a watchdog reset | `watchdog_reset()` on offset 0 | agree | ^[raw/docs/taito-f3/fio.txt; raw/emu-source/mame-0.289/taito_f3.cpp L74-L76] |

## Notes-only

- Hardware signal timing: sync pulse lengths, equalizing pulses and their errors, power-on and drop-in/drop-out traces (`vtiming*.txt`) ^[raw/docs/taito-f3/website/video.html; raw/docs/taito-f3/vtiming_startup.txt].
- Blur/drive details of the FDA and the 13 MHz clock into the FDP ^[raw/docs/taito-f3/website/fda.html; raw/docs/taito-f3/fdp.txt].

## MAME-only

- Game-specific hooks (`m_game == KIRAMEKI` sound bankswitch, bootleg `bubsympb` OKI map) ^[raw/emu-source/mame-0.289/taito_f3.cpp L117-L131, L202-L236].
- Sound CPU reset held on machine reset ("qtheater relies on it") ^[raw/emu-source/mame-0.289/taito_f3.cpp L436-L441].

## Runtime consequence

Hypothesis: the biggest practical gap is interrupt 5. MAME never fires it, so games that depend on it
(those writing `0x278B`, per MAME's TODO list) are not exercised by MAME-based traces; the notes'
timing formula `0x4000 − rate·8 + 32` CPU cycles is the only documented source for emulating it. The
interrupt 3 delay and CPU clock are also open until measured against this runtime.
