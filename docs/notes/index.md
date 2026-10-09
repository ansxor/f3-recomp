# Index

- [[SCHEMA]] — conventions, system, games, evidence kinds, tags, submodule sources

## Games

- [[games/landmakrj]] (hypothesis) — Land Maker: tool setup, input scripts, routine index
- [[games/commandw]] (hypothesis) — Command War: tool setup, findings awaiting ingest

## Routines

- [[routines/landmakrj-0008e20c-round-task]] (hypothesis) — round task; the round-end gate opens at 0x401f54 ≥ 10 (or ≥ 3)

## Hardware: board and CPU side

- [[hardware/board]] (hypothesis) — chip inventory across board revisions, RAM chips, jumpers, reset/watchdog/MB3771 power-monitor path
- [[hardware/clocks]] (hypothesis) — 26.686 / 30.47618 / 16 MHz oscillators, pixel and half-pixel clocks, 15.238 MHz CPU net, ESP and DUART clocks
- [[hardware/cartridge]] (hypothesis) — connector ports A–D: CPU bus and program ROM, OTIS sample bus, FDP graphics ROM bus, audio CPU bus
- [[hardware/pal-decoders]] (hypothesis) — PAL equations: address decode (pal1/2/15), vblank sequencing (pal7), graphics ROM and audio PALs
- [[hardware/fcm]] (hypothesis) — TC0660FCM role, decoded ranges, AS/DSACK gating through ic64/ic65/ic9
- [[hardware/memory-map]] (hypothesis) — 68EC020 bus map with answering chip, mirrors, freeze-on-unmapped, MAME f3_map() side by side

## Hardware: video

- [[hardware/fdp]] (hypothesis) — TC0630FDP: layers, graphics RAM map 0x600000–0x66001F, scroll/settings registers, framebuffer
- [[hardware/glossary]] (hypothesis) — 12Me21's video terms: half-pixel, priority cell, blend mode/select, subglobal scroll
- [[hardware/sprites]] (hypothesis) — 8-word sprite entry, zoom, global/subglobal scroll, block (tessellation) controls, groups, MAME sprite lag
- [[hardware/sprite-command-word]] (hypothesis) — sprite settings word: flipscreen, bpp, bank select, framebuffer clear
- [[hardware/tilemaps]] (hypothesis) — PF1–4 tile RAM decode, tile word bits, global scroll, rowscroll/colscroll/zoom/palette add
- [[hardware/pivot-layer]] (hypothesis) — pivot text vs pixel mode, text/font/pixel RAM layouts, bottom-half reuse, pivot port
- [[hardware/line-ram]] (hypothesis) — line RAM latch table at 0x620000, sections, alt bank, latch semantics
- [[hardware/line-ram-registers]] (hypothesis) — bit tables for the 30 line registers (4400–B600), per-source comparison
- [[hardware/fda]] (hypothesis) — TC0650FDA: color RAM format, half-pixel blend of two color indexes, blur/mode signals
- [[hardware/priority-and-blend]] (hypothesis) — 16×2 priority-cell model, mix words, blend modes, alpha select, background color
- [[hardware/clip-and-mosaic]] (hypothesis) — four clip windows, per-layer enable/invert bits, combine rules, mosaic
- [[hardware/highcolor-layer]] (hypothesis) — hidden 16bpp "mystery" layer reusing tilemap data; registers 6000/6400/7000
- [[hardware/shadow-mode]] (hypothesis) — shadow modes 2/3: palette-bit modification between neighbouring priorities
- [[hardware/video-timing]] (hypothesis) — 6.6715 MHz pixel clock, 432×262 (~58.94 Hz), sync table, frame pulse, startup frames

## Hardware: interrupts, I/O, sound

- [[hardware/interrupts]] (hypothesis) — main CPU IRQ levels 2/3/5, int5 timer at 0x4C0000, MAME's model
- [[hardware/fio]] (hypothesis) — TC0640FIO port map: inputs, dials, coin counters/lockouts, EEPROM, watchdog/reset
- [[hardware/sound]] (hypothesis) — sound 68000 map, 0xC00000 shared RAM, sound reset, OTIS bank counter, ES5510, DUART

## Quirks

- [[quirks/frame-pulse-interrupt-window]] (hypothesis) — frame pulse high end of line 255 to end of line 4; int2→int3 ~712 µs vs MAME 10000 cycles
- [[quirks/interrupt5-timer-interval]] (hypothesis) — write to 0x4C0000 with bit 13 set starts a periodic level-5 IRQ; MAME ignores it
- [[quirks/video-blank-bit-invalid-sync]] (hypothesis) — 0x0002 to 0x66001E blanks video and breaks vsync
- [[quirks/sprite-enable-scrolls-value-2]] (hypothesis) — sprite "enable scrolls" value 2 adds both scrolls (MAME: neither)
- [[quirks/sprite-set-scroll-affects-same-sprite]] (hypothesis) — a set-scroll sprite is itself offset by the new scroll
- [[quirks/sprite-odd-bank-lasts-one-frame]] (hypothesis) — odd sprite-RAM banks cannot stay active two frames running
- [[quirks/lineram-latch-persists-across-frames]] (hypothesis) — latched line-RAM values persist across frames (MAME resets)
- [[quirks/tilemap-y-zoom-pf1-pf3-swapped]] (hypothesis) — PF1/PF3 Y zoom live in each other's 8200/8600 word
- [[quirks/priority-conflict-shows-background]] (hypothesis) — same-cell priority conflict shows background color (MAME: palette 0)
- [[quirks/shadow-mode-3-pivot-sets-bit12]] (hypothesis) — shadow 3 sets palette bit 12 on pivot, clears it on sprites
- [[quirks/mosaic-counter-resets-before-right-edge]] (hypothesis) — mosaic phase restarts 2 px before the right edge (MAME: column 318)

## Comparisons

- [[comparisons/hardware-vs-mame-video]] (hypothesis) — notes vs MAME 0.289: sprites, tilemaps/pivot, line RAM, blend/priority/clip/mosaic/colour
- [[comparisons/hardware-vs-mame-system]] (hypothesis) — notes vs MAME 0.289: clocks/raster, interrupts 2/3/5, address map, FIO/input
