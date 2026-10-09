---
title: F3 PAL decoders
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [pcb, memory-map, cpu, audio, vblank]
sources: [raw/docs/taito-f3/pal1.txt, raw/docs/taito-f3/pal2.txt, raw/docs/taito-f3/pal6.txt, raw/docs/taito-f3/pal7.txt, raw/docs/taito-f3/pal8.txt, raw/docs/taito-f3/pal9.txt, raw/docs/taito-f3/pal12.txt, raw/docs/taito-f3/pal15.txt, raw/docs/taito-f3/pals.txt, raw/docs/taito-f3/mbv.txt, raw/docs/taito-f3/address-mapping.txt]
games: []
addresses: [0x00000000, 0x00400000, 0x00600000, 0x00C00000, 0x00C80000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/pal1.txt, note: "dumped D77-01 equations and pin roles"}
  - {kind: doc, ref: raw/docs/taito-f3/pal2.txt, note: "dumped D77-02 equations (C-region, DSACK, RAM OE/RW)"}
  - {kind: doc, ref: raw/docs/taito-f3/pal15.txt, note: "dumped D77-15 equations (cartridge program ROM)"}
contradictions:
  - "pal1 pin 22 is labelled 'map upper quadrant (C-F)' / 'addr = 11*' in comments, but the equation printed for o22 uses /i5 & /i6 (cpu.a22=0, cpu.a23=0, i.e. 00*), the same decode as o18 without AS. Equation text and annotation disagree (notes disagree with each other)."
  - "pal2's prose comment says that with the shared-RAM busy input i14 low 'neither DSACK is asserted and the cpu waits', but the printed equation for DSACK0 asserts it when i14 is low (term '... & /i14')."
supersedes: []
---

# F3 PAL decoders

The F3 uses PALs to decode the main CPU bus (pal1, pal2), program ROM on the cartridge (pal15), video sequencing (pal7, pal9, pal10), and the audio side (pal3–pal6, pal8, pal12). Some were dumped with equations; others only have pin roles. Input numbering below uses the PAL's package pin numbers. The resulting address map is in [[hardware/memory-map]]; the FCM that receives the decoded selects is in [[hardware/fcm]].

CPU signal assignments shared by pal1 and pal2 (pins 1–13 and 23): 1=a18, 2=a19, 3=a20, 4=a21, 5=a22, 6=a23, 7=AS, 8=DS, 9=R/W, 10=FC0, 11=FC1, 13=FC2, 23=a17 (24=VCC, 12=GND). ^[raw/docs/taito-f3/fcm.txt] The notes' "FC=?01 or ?10" means FC1:FC0 = 01 (user data) or 10 (user program). ^[raw/docs/taito-f3/pal1.txt]

## pal1 (D77-01, PALCE20V8, `ic2` New / `ic28` Single)

Address mapping for the main CPU; outputs go to the FCM and FDP. ^[raw/docs/taito-f3/pal1.txt] ^[raw/docs/taito-f3/mbv.txt]

| Output | Dest | Decodes (per printed equation) | Notes' label |
|---|---|---|---|
| o15, o16 | — | passthrough of a20, a19 | ^[raw/docs/taito-f3/pal1.txt] |
| o18 | unused? | a22=a23=0, AS low, FC=?01/?10 (`00*`) | "map program rom space (0x[0-3]?????) - maybe used in single board?" ^[raw/docs/taito-f3/pal1.txt] |
| o19 | fcm.143 | a23..a20 = 0100, FC=?01/?10 (`0x4?????`) | "map misc ram space" ^[raw/docs/taito-f3/pal1.txt] |
| o20 | fcm.142 | a23..a20 = 0110 (`0x6?????`), no AS or FC term ("unsafe") | "map graphics ram space" ^[raw/docs/taito-f3/pal1.txt] |
| o21 | fdp.1 | identical to o20 | ^[raw/docs/taito-f3/pal1.txt] |
| o22 | fcm.141 | printed: a22=a23=0, FC=?01/?10 | labelled "map upper quadrant (0xC-F)" — contradiction, see frontmatter ^[raw/docs/taito-f3/pal1.txt] |

Pin 14 is tied to the power-monitor reset net (pal5.23, mb3771.reset, fio.115). ^[raw/docs/taito-f3/pal1.txt] Pins 1, 8, 9, 13 are listed as unused in the equations. ^[raw/docs/taito-f3/pal1.txt]

## pal2 (D77-02, PALCE20V8, `ic3` New / `ic29` Single)

Controls the `0xC?????` region, the work-RAM OE/RW strobes, and drives DSACK there. ^[raw/docs/taito-f3/pal2.txt]

| Output | Dest | Equation summary |
|---|---|---|
| o15 | pal8.4 | a23..a19 = 11001 (`0xC80000-0xCFFFFF`), AS low, FC=?01/?10 — audio reset/control port ^[raw/docs/taito-f3/pal2.txt] |
| o16 | ic31.1 (shared RAM CS left), pal6.8 | a23..a19 = 11000 (`0xC00000-0xC7FFFF`), AS low, FC=?01/?10 ^[raw/docs/taito-f3/pal2.txt] |
| o17 | ??? | a23 passthrough ^[raw/docs/taito-f3/pal2.txt] |
| o18 = DSACK1 | cpu | asserted when a23..a19 = 11001, AS and DS low; high-Z unless a23..a20 = 1100 ^[raw/docs/taito-f3/pal2.txt] |
| o19 = DSACK0 | cpu | asserted for 11001 with AS/DS, or for 1100 with AS/DS and i14 low; high-Z unless 1100 ^[raw/docs/taito-f3/pal2.txt] |
| o20 | work RAM OE | AS low and R/W high (read) ^[raw/docs/taito-f3/pal2.txt] |
| o21 | work RAM R/W | AS low, DS low, R/W low, FC0=1, FC1=0 (write in data space only) ^[raw/docs/taito-f3/pal2.txt] |
| o22 | fcm.126 | FC2:FC0 = 111 (CPU space, i.e. interrupt acknowledge) ^[raw/docs/taito-f3/pal2.txt] |

Reading of the DSACK terms (the notes' own): the sound reset port answers with both DSACKs (32-bit); shared RAM answers only DSACK0 (8-bit port), and i14 (probably the shared-RAM busy from pal6.12) makes it wait. ^[raw/docs/taito-f3/pal2.txt] The notes comment that this decode does not check AS in places and could mistakenly grab DSACK on rapid successive accesses. ^[raw/docs/taito-f3/pal2.txt]

Software-visible consequences (all `doc`-level): work-RAM writes are only enabled in data space (FC=?01) — the notes are unsure whether any other access type can write; reads are not affected. ^[raw/docs/taito-f3/pal2.txt] DSACK for all other regions comes from the FCM (not pal2). ^[raw/docs/taito-f3/address-mapping.txt] ^[raw/docs/taito-f3/fcm.txt]

## pal15 (D77-15, cartridge, program ROM)

- Inputs: a17..a21 on 1–5, a22/a23 on 6–7, AS 8, DS 9, FC0–2 on 16/15/14, reset 13. ^[raw/docs/taito-f3/pal15.txt]
- `/o17` (program ROM OE) = !a22 & !a23 & !AS & (FC1:FC0 = 10 or 01); `o18` (ROM A17) = cpu.a19; `o19` (ROM A18) = cpu.a20 (address bits shifted by 2); `o12` = reset passthrough, "unused??". ^[raw/docs/taito-f3/pal15.txt]
- Hypothesis (the wiki's reading of the pal15 terms): cpu.a21 is not in any term, so the `00*` decode covers 0x000000–0x3fffff with a 2 MB ROM address range; see [[hardware/cartridge]] and [[hardware/memory-map]]. ^[raw/docs/taito-f3/pal15.txt]

## Video: pal7 and pal9/pal10

- **pal7 (D77-07, `ic33`, PALCE16V8Q-15)**: pins 1/2 are fdp.120 (video sync, pulled down by R12), pin 3 fdp.118, pin 19 → fdp.121; pins 14–17 give 8/4/2/1 high pulses during vblank (not connected). fdp.118 goes high near the end of scanline 255 and low near the end of scanline 4 (11 scanlines total, when the CPU interrupts happen); fdp.121 goes low at the end of vsync for 232 pixels. The notes do not know what fdp.121 does inside the FDP. ^[raw/docs/taito-f3/pal7.txt] See [[hardware/video-timing]] and [[hardware/interrupts]].
- **pal9 (D77-09)**: sprite/tile ROM control. Inputs: pal9.4 latched graphics a20 (unused), pal9.5–7 latched a21–a23, pal9.8 = 7 MHz, pal9.9 = 13 MHz, pal9.11 = 3.33 MHz. Equations as printed: `/o12 = i11`, `/o13 = i11 + /i8 + i9` (tile address latch enable, per pal9.13), `/o14 = /i8` (tile data / shared data buffer OE), `/o15..18 = {i5,i6,!i7 patterns} & !i11` (sprite ROM OE groups 3..0), `/o19 = /i11 + /i8 + i9` (sprite address latch enable). The notes' oscilloscope check found the first sequencing table "almost correct" with some things inverted, so treat polarities with care. ^[raw/docs/taito-f3/pal9.txt]
- **pal10 (D77-10)**: tile ROM control: `o16 = i4 & !i5 & !i6 & !i8`, `o17 = !i4 & !i5 & !i6 & !i8` (tile ROM group OEs 2 and 1), `o19` is the tile ROM byte-mode line. Tiles only use a 23-bit address; address space is 1/4 as large as sprites'. ^[raw/docs/taito-f3/pal9.txt] See [[hardware/sprites]] and [[hardware/cartridge]].

## Audio side

- **pal3/pal4/pal5** (PALCE20V8, `ic23/ic24/ic25`): audio-CPU address mapping; pal4 and pal5 dumps are missing. pal5 pin 23 is also the power-monitor reset; pal5 pins 16/20 are the chip selects of ic30/ic31 (right side); pal4.18/pal6.3 volume-control CE. ^[raw/docs/taito-f3/pals.txt] ^[raw/docs/taito-f3/pal6.txt] ^[raw/docs/taito-f3/mbv.txt]
- **pal6 (D77-06, `ic29`)**: "undumped"; pin 8 = ic31.1/pal2.16 (shared-RAM CS left), pin 12 → pal2.14 (shared-RAM busy, pulled up with R7 via ic31.3), pins 2/4 = chip selects right, pin 9 = audio DS, pin 5 = OTIS SERWCLK, pin 15 = ESP WCLK, pin 1 = 15 MHz clock. ^[raw/docs/taito-f3/pal6.txt]
- **pal8 (`ic43`)**: inputs cpu.reset, ESP LRCLK, DUART OP6, pal2.15 (main-CPU write to sound reset port), audio A1, ic30 IO1/IO2, otis.e, main-CPU a8 (?), otis CAS; outputs/IO: ESP HALT (dr2.7/esp.36), audio CPU HALT, RESET and volume DSEL. address-mapping.txt says pal8 checks a8 to set or clear the audio reset line (see `0xC80000` vs `0xC80100`). ^[raw/docs/taito-f3/pal8.txt] ^[raw/docs/taito-f3/address-mapping.txt] See [[hardware/sound]].
- **pal12 (D77-12, cartridge)**: audio ROM mapping. `/o18` (ROM OE) is asserted when AS is low and either (audio a23..a19 = 11000, i.e. `0xC00000-0xC7FFFF` on the audio CPU, with FC=?10 or ?01) or (a23..a19 = 00000 with FC=110, supervisor program, i.e. the low vector area); `o19` (ROM A17) = audio a18; `o12` (A18) has no equation and is "unused on this board". The `0xC00000` window matches MAME's `bankr("cpubank1")` at `0xc00000-0xc1ffff` on the audio CPU. ^[raw/docs/taito-f3/pal12.txt] ^[raw/emu-source/mame-0.289/taito_en.cpp]
