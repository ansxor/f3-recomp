---
title: Main CPU (68EC020) bus map
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [memory-map, cpu, pcb, io, undocumented]
sources: [raw/docs/taito-f3/website/memory.html, raw/docs/taito-f3/website/memory layout.html, raw/docs/taito-f3/address.txt, raw/docs/taito-f3/address-mapping.txt, raw/docs/taito-f3/mem.txt, raw/docs/taito-f3/unified.txt, raw/docs/taito-f3/pal1.txt, raw/docs/taito-f3/pal2.txt, raw/emu-source/mame-0.289/taito_f3.cpp]
games: []
addresses: [0x00000000, 0x00300000, 0x00400000, 0x00440000, 0x00480000, 0x004A0000, 0x004C0000, 0x004E0000, 0x00600000, 0x00610000, 0x0061C000, 0x0061E000, 0x00620000, 0x00630000, 0x00660000, 0x00680000, 0x006E0000, 0x00C00000, 0x00C80000, 0x00C80100]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/memory.html, note: "address-bit map with sizes, widths, notes from hardware tests (freeze on unmapped access)"}
  - {kind: doc, ref: raw/docs/taito-f3/mem.txt, note: "raw probe results: which ranges freeze, mirrors, address-reflect"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "f3_map() main CPU map"}
contradictions:
  - "Palette RAM: mem.txt says 440000-45FFFF '16 bit'; memory.html says 32-bit with the top byte of each long-word unconnected, and a $8000-byte real size."
  - "Work RAM mirror: memory.html gives the second window as [0100 111a ...] (0x4e0000), mem.txt lists 4E0000-4FFFFF as a mirror, and MAME mirrors 0x400000-0x41ffff at +0x20000 (0x420000-0x43ffff); the 0x4e0000 window and the 0x420000 window are different mirrors."
  - "Timer register 0x4c0000: memory.html says 16-bit, mem.txt says '1 bit', MAME maps four bytes 0x4c0000-0x4c0003 write-only with a 16-bit handler."
  - "Graphics RAM size: address.txt/memory.html show 0x40000 bytes at 0x600000 mirrored through bit 19 (mem.txt: 680000-6B0000 mirror); MAME splits 0x600000-0x63ffff into six handlers (sprite, playfield, text, charram, line RAM, pivot) with no 0x680000 mirror."
  - "Address reflect vs timer register: address.txt and memory.html give both `[0100 110. ...]` bit patterns (0x4c0000 region), yet mem.txt places address reflect at 0x480000 (`0100 100.`); the printed address-reflect pattern is probably a typo."
supersedes: []
---

# Main CPU (68EC020) bus map

This is the bus map of the 68EC020 program bus on the F3 motherboard. It is hand-traced: 12Me21's tables combine PAL equations ([[hardware/pal-decoders]]), FCM behaviour ([[hardware/fcm]]) and read/write tests on hardware. MAME's `f3_map()` is shown for comparison. The FDP's internal layout is in [[hardware/fdp]]; register detail for each device is in [[hardware/fio]], [[hardware/line-ram]], [[hardware/sound]]. This is `doc`/`emu-source` evidence only.

Bit legend used in the notes: `0`/`1` must match, `.` doesn't matter, `a` real address bit, `A` byte-select within the data bus. ^[raw/docs/taito-f3/website/memory.html]

## Map

| Start | Notes' end / real size | Name (address bits) | Chip | Width / R/W | MAME `f3_map()` |
|---|---|---|---|---|---|
| `0x00000000` | `0x003fffff`, program ROM "$200000?" | Program ROM `[00aa aaaa aaaa aaaa aaaa aaAA]` | cartridge ROM via pal15 | 32-bit, R | `0x000000-0x1fffff` ROM ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/address.txt] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00300000` | — | (Kirameki audio ROM bank switch, `0x30xxxx`) | cartridge | write | `0x300000-0x30007f` write handler ^[raw/docs/taito-f3/website/memory layout.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00400000` | `0x0041ffff` (0x20000) | Work RAM `[0100 00.a aaaa aaaa aaaa aaAA]` | FCM + four TC51832 | 32-bit, R/W | RAM `0x400000-0x41ffff`, mirror `+0x20000` ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00440000` | `0x00447fff` (0x8000) | Color/palette RAM `[0100 010. .aaa aaaa aaaa aaAA]` | three IS61C64AH | 32-bit (top byte unconnected), R/W | RAM `0x440000-0x447fff` + `palette_24bit_w` ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00480000` | `0x004800ff` (0x100) | "Address reflect" `[0100 110. .... .... aaaa aaaa]` | FCM? | 8-bit?, R? | not mapped ^[raw/docs/taito-f3/website/memory.html] |
| `0x004a0000` | `0x004a001f` (0x20) | FIO registers `[0100 101. .... .... ...a aaaa]` | TC0640FIO | 8-bit, R/W | `0x4a0000-0x4a001f` `f3_control_r/w` ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x004c0000` | 2 bytes | Timer register `[0100 110. .... .... .... ...A]` | FCM | 16-bit?, R/W | `0x4c0000-0x4c0003` write `f3_timer_control_w` ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x004e0000` | `0x004fffff` | Work RAM mirror `[0100 111a aaaa aaaa aaaa aaAA]` | as work RAM | 32-bit | not mapped ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/mem.txt] |
| `0x00600000` | `0x0063ffff` (0x40000) | Graphics RAM `[0110 .0aa aaaa aaaa aaaa aaaA]` | FDP's two LH5P8128 | 16-bit, R/W | `0x600000-0x63ffff` split, see below ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00660000` | `0x0066001f` (0x20) | FDP registers `[0110 .110 0000 0000 000a aaaA]` | TC0630FDP | 16-bit, R/W | write-only `0x660000-0x66000f` control_0, `0x660010-0x66001f` control_1 ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00680000` | `0x006b0000` | graphics RAM mirror | as above | | not mapped ^[raw/docs/taito-f3/mem.txt] |
| `0x006e0000` | — | mirror of FDP regs, then freeze | | | not mapped ^[raw/docs/taito-f3/mem.txt] |
| `0x00c00000` | `0x00c007ff` (0x800) | Dual-port RAM `[1100 0... .... .aaa aaaa aaaa]` | MB8421 (left port) | 8-bit, R/W | `0xc00000-0xc007ff` `taito_en:dpram` left port ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00c80000` | — | Sound reset clear `[1100 1... .... ...0 .... ....]` | pal2 → pal8 | 32-bit, W? | `0xc80000-0xc80003` write: audio CPU reset line CLEAR ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |
| `0x00c80100` | — | Sound reset assert `[... ...1 ....]` | pal2 → pal8 | 32-bit, W? | `0xc80100-0xc80103` write: reset line ASSERT ^[raw/docs/taito-f3/website/memory.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] |

Everything else is unmapped. Per the hardware tests, accessing an unmapped address freezes the CPU rather than raising a bus error. ^[raw/docs/taito-f3/website/memory.html] mem.txt lists probe results of "freeze" at `0x460000`, `0x470000`, `0x500000`, `0x580000`, `0x640000`, `0x650000`, `0x660020+`, `0xD00000`, `0xE00000`, `0xF00000`. ^[raw/docs/taito-f3/mem.txt] Hypothesis for the runtime: an access outside the table above is a game bug, not a recoverable fault.

## Region details (from the notes)

- **Program ROM**: pal15 decodes `00*` with AS low and FC=?01/?10 (see [[hardware/pal-decoders]]); size and mapping depend on the cartridge, "but i think all extant games are basically the same", except Kirameki, which detects writes near `0x300000` and switches audio-CPU ROM banks (marked TODO in the notes). Writes to ROM do nothing. ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/pal15.txt] See [[hardware/cartridge]].
- **Color RAM**: "mapped as 32-bit, but the first (highest) byte of each long-word is not connected to ram". unified.txt guesses the stored word as 24 bits `[xxxx xxxx rrrr rrrr gggg gggg bbbb bbbb]`, with a 15-bit alternative `[.... rrrr gggg bbbb rgbx]` marked "i think". Use [[hardware/fda]]/[[hardware/priority-and-blend]] for what the palette does. ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/unified.txt]
- **Address reflect** (`0x480000`): reads return the low byte of the address; writes are untested; "i'm not sure what the point of this is". mem.txt: "480000-49FFFF - returns lower byte of address". ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/mem.txt]
- **FIO**: 5 address bits (`0x20` bytes); the reading of register offsets is in [[hardware/fio]]. MAME's read handler returns input ports for offsets 0–5 and `0xffffffff` otherwise; writes offset 0 = watchdog, 1 = coin counters/lockouts, 4 = EEPROM. ^[raw/docs/taito-f3/mem.txt] ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- **Timer register** (`0x4c0000`): handled by FCM "i assume entirely"; the notes call it "timer interrupt". MAME's handler comment says several games configure a timer-based pseudo-hblank interrupt 5 here at POST. ^[raw/docs/taito-f3/address-mapping.txt] ^[raw/emu-source/mame-0.289/taito_f3.cpp] See [[hardware/interrupts]].
- **Graphics RAM** (`0x600000-0x63ffff`): the layout page sketches `60xxxx` sprite RAM, `61xxxx` playfields / text RAM / char (font) RAM, `62xxxx` "latches pivot port? lineram", `63xxxx` pivot RAM. MAME's split: sprite `0x600000-0x60ffff`, playfield `0x610000-0x61bfff`, text `0x61c000-0x61dfff`, char `0x61e000-0x61ffff`, line RAM `0x620000-0x62ffff`, pivot `0x630000-0x63ffff`. ^[raw/docs/taito-f3/website/memory layout.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] The FDP's own view is in [[hardware/fdp]], [[hardware/sprites]], [[hardware/tilemaps]], [[hardware/pivot-layer]] and [[hardware/line-ram]].
- **FDP registers**: occupy exactly `0x20` bytes and "are not mirrored, unlike everything else (e.g. reading from $660020 will freeze)" (memory.html). mem.txt lists `660000-66001F` "scroll registers", `660020+` freeze, and `6E0000` "mirror of scroll registers, followed by freeze". ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/mem.txt] The two files disagree about whether a mirror exists at `0x6e0000` (notes-internal contradiction, unresolved; memory.html's `.` bit 19 in the pattern would allow a mirror at `0x6e0000` relative to `0x660000`, Hypothesis).
- **Dual-port RAM** (`0xC00000`): 11 address bits, 8-bit on a 32-bit bus, shared with the audio CPU through an MB8421; pal2 supplies DSACK1-negated/DSACK0-asserted for this region and honours the busy line from pal6. The decode covers `0xC00000-0xC7FFFF`. ^[raw/docs/taito-f3/mem.txt] ^[raw/docs/taito-f3/pal2.txt] ^[raw/docs/taito-f3/address-mapping.txt] See [[hardware/sound]].
- **Sound reset** (`0xC80000` region): pal2 decodes `11001*` and pal8 checks A8 to clear or assert the audio CPU reset: bit 8 clear = "Sound Reset Clear" (`0xC80000`), bit 8 set = "Sound Reset Assert" (`0xC80100`). Reads of this region return `18, FC, 00, FF` repeating (`0xC80000-0xCFFFFF`); the notes want to test further what the read value means. ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/mem.txt] ^[raw/docs/taito-f3/address-mapping.txt]

## Where the decode comes from

pal1 outputs `0x4?` → fcm.143, `0x6?` → fcm.142 and fdp.1, and "upper quadrant" → fcm.141; pal2 handles `0xC?` (shared RAM and sound reset) including DSACK. The FCM sees A0–A7 and A15–A19; FDP A0–A14; FIO A0–A4. ^[raw/docs/taito-f3/address-mapping.txt] ^[raw/docs/taito-f3/pal1.txt] ^[raw/docs/taito-f3/pal2.txt] Hypothesis: the `.` (don't-care) bits in the patterns above, such as bit 19 in graphics RAM, are what produce the mirrors. See [[hardware/fcm]].

## Differences from MAME (summary)

- MAME maps `0x000000-0x1fffff` ROM only; the hardware decode (a22=a23=0, a21 unused) also answers `0x200000-0x3fffff` (hypothesis, from pal15 equation). ^[raw/emu-source/mame-0.289/taito_f3.cpp] ^[raw/docs/taito-f3/pal15.txt]
- MAME has no handler for `0x480000` (address reflect), `0x4e0000` (work-RAM mirror), `0x680000`/`0x6e0000` mirrors, nor the `0xC80000` read value. ^[raw/emu-source/mame-0.289/taito_f3.cpp] Hypothesis: unmapped accesses in MAME do not freeze the CPU (MAME's default unmapped handling; not shown in the raw files).
- MAME's FDP register handlers are write-only (`control_0_w`/`control_1_w`); memory.html lists the registers as R/W without detailing read values. ^[raw/emu-source/mame-0.289/taito_f3.cpp] ^[raw/docs/taito-f3/website/memory.html]
- The bubsympb bootleg map in MAME reuses the same layout but adds OKI registers at `0x4a001d/0x4a001f` and plain RAM at `0xc00000`; it is not a standard F3. ^[raw/emu-source/mame-0.289/taito_f3.cpp]
