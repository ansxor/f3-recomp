---
title: F3 motherboard chip inventory
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [pcb, reset, cpu, memory-map]
sources: [raw/docs/taito-f3/mbv.txt, raw/docs/taito-f3/chips.txt, raw/docs/taito-f3/parts.txt, raw/docs/taito-f3/list.txt, raw/docs/taito-f3/graphics-ram-chips.txt, raw/docs/taito-f3/ic9.txt, raw/docs/taito-f3/website/schematics.html, raw/docs/taito-f3/website/connectors.html, raw/emu-source/mame-0.289/taito_f3.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/mbv.txt, note: "designator table across three board revisions (New / Single / Orange), part numbers and roles"}
  - {kind: doc, ref: raw/docs/taito-f3/list.txt, note: "net lists traced from the PCB: reset path, clock buffers, ic64/ic65"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "MAME's PCB ASCII diagram and device list"}
contradictions:
  - "MAME's machine config clocks the 68EC020 from a 16 MHz XTAL (F3_MAIN_CLK), while its own PCB comment and 12Me21's cartridge pinout give 15.23809 MHz (30.47618 MHz / 2). See [[hardware/clocks]]."
supersedes: []
---

# F3 motherboard chip inventory

Summary of the chips on the F3 main board, from 12Me21's PCB tracing. Full pin lists live in the raw files; this page only names parts and roles. For the bus these chips share see [[hardware/memory-map]]; for what plugs in see [[hardware/cartridge]]. All of this is secondhand `doc` evidence (see [[SCHEMA]]).

## Designators across revisions

`mbv.txt` has three columns of designators: "New" (the newest motherboard, used by the schematics), "Single" (the single-board F3 layout) and "Orange" (a third board). Designators below are the "New" column; the Single/Orange designator is given where the notes list one, and `?` marks the notes' own uncertainty. ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/website/schematics.html]

| New | Single | Name | Part | Role (notes' wording) |
|---|---|---|---|---|
| ic10 | ic44 | cpu | MC68EC020FG16 | main CPU ^[raw/docs/taito-f3/mbv.txt] |
| ic13 | ic43 | fdp | TC0630FDP | graphics rendering ^[raw/docs/taito-f3/mbv.txt] — see [[hardware/fdp]] |
| ic41 | ic78 | fda | TC0650FDA | color blending ^[raw/docs/taito-f3/mbv.txt] — see [[hardware/fda]] |
| ic4 | ic39 (Orange ic21) | fcm | TC0660FCM | misc: address mapping, CPU/FDP comms, interrupts ^[raw/docs/taito-f3/mbv.txt] — see [[hardware/fcm]] |
| ic37 | ic63 (only one older designator listed; column ambiguous) | fio | TC0640FIO | I/O ports ^[raw/docs/taito-f3/mbv.txt] — see [[hardware/fio]] |
| ic26 | ic1 (Orange ic19) | otis | OTISR2 (ES5505) | audio sample playback ^[raw/docs/taito-f3/mbv.txt] |
| ic42 | ic19 (Orange ic15) | esp | ESPR6 (ES5510) | audio signal processing ^[raw/docs/taito-f3/mbv.txt] |
| ic27 | ic4 (Orange ic16) | — | Ensoniq 5701 | OTIS/ESP/audio-CPU glue, clock generation ^[raw/docs/taito-f3/mbv.txt] |
| ic32 | ic11 | acpu | MC68EC000FN16 (older: MC68000P12F) | audio CPU ^[raw/docs/taito-f3/mbv.txt] |
| ic28 | ic21 (Orange ic9) | duart | MC68681P | serial communication ^[raw/docs/taito-f3/mbv.txt] |
| ic36 | ic62 | — | 93C46CB1 | serial EEPROM ^[raw/docs/taito-f3/mbv.txt] |
| ic35 | ic55 | — | MB3771 | power-supply monitor ^[raw/docs/taito-f3/mbv.txt] |
| ic52 | ic79 | dac | TDA1543 | audio DAC ^[raw/docs/taito-f3/mbv.txt] |

`mbv.txt` lists only two designators for ic37, so which older board ic63 belongs to is unclear. ^[raw/docs/taito-f3/mbv.txt]

Support logic (New designator → part → role): ic1 MC74HC08AN CPU clock driver; ic9 SN74LS08N (sprite-framebuffer-related AND gates); ic18 MC74F163AN DUART clock divider; ic19 MC74F74N video clock divider; ic20 MC74F244N clock driver; ic21 MC74F74N and ic22 MC74F161AN voice-bank counter; ic64 MC74F74N and ic65 SN74F04N CPU/FDP access gating ("??" in the notes). ^[raw/docs/taito-f3/mbv.txt] The PALs (ic2, ic3, ic23–ic25, ic29, ic33, ic43) are covered in [[hardware/pal-decoders]].

## RAM chips

| Use | New designators | Part | Notes |
|---|---|---|---|
| Main CPU work RAM | ic14–ic17 | TC51832ASPL-85, 32K×8 pseudo-static | four chips, one per byte lane; chip selects come from FCM pins 102–105 ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/fcm.txt] |
| Graphics RAM | ic11, ic12 ("gram1/gram2") | LH5P8128-80 (TC518128A-class, 128K×8 pseudo-static) | both driven by FDP address pins, shared CE1 (fdp.32) and OE (fdp.123) ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/chips.txt] ^[raw/docs/taito-f3/graphics-ram-chips.txt] |
| Sprite framebuffer | ic5–ic8 | fast-page-mode DRAM (HM511664 64K×16; TC511664BJ-80 is a confirmed working replacement) | ^[raw/docs/taito-f3/mbv.txt] ^[raw/docs/taito-f3/chips.txt] |
| Palette RAM | ic38–ic40 | IS61C64AH-20N, 8K×8 SRAM | three chips ^[raw/docs/taito-f3/mbv.txt] |
| Audio CPU RAM | ic44, ic45 | TC51832ASPL-85 | ^[raw/docs/taito-f3/mbv.txt] |
| CPU/audio shared RAM | ic31 | MB8421-90LP 2K×8 dual-port | see [[hardware/sound]] ^[raw/docs/taito-f3/mbv.txt] |
| OTIS bank list | ic30 | MB8421-90LP | "voice bank list" ^[raw/docs/taito-f3/mbv.txt] |
| ESP RAM | ic51 | fast-page-mode RAM | ^[raw/docs/taito-f3/mbv.txt] |

The notes say colour RAM is "mapped as 32-bit, but the first (highest) byte of each long-word is not connected to ram". ^[raw/docs/taito-f3/website/memory.html] Hypothesis: that unconnected byte is why three 8-bit chips suffice; the files do not say how the chips map onto R/G/B.

TC518128A pin list note: the notes treat LH5P8128 as "same?" as TC518128A, with a `~RFSH` pin 1 that gram2 ties to 5 V. ^[raw/docs/taito-f3/chips.txt] ^[raw/docs/taito-f3/graphics-ram-chips.txt]

## Old (pre-"New") board contents

`mbv.txt` also lists chips found only on the older board: eight 74F244/74F373 buffer/latch pairs, mask-ROM sockets (D53-xx graphics, sample and program ROMs), and several 20L8B/16L8B PALs (D29-xx, D53-12, D49-12-1, "probably grp rom mapping"). Ic60 on the old board is said to be the probable graphics-ROM mapping PAL. ^[raw/docs/taito-f3/mbv.txt] Those ROM-side parts moved to the cartridge — see [[hardware/cartridge]].

## Jumpers

- jp1 (Single: jp6): selects the 7 MHz clock input for the FDA; jp2: FIO pin 4; jp3: joystick/dial pull resistors; jp4: "?? esp voltage?". ^[raw/docs/taito-f3/mbv.txt]
- JP3 sets joystick inputs to pull-up (buttons/joysticks) or pull-down (rotary dials). ^[raw/docs/taito-f3/website/connectors.html]

## Reset, watchdog and power monitor

- The main CPU reset (active low) is generated by FIO in response to the watchdog or the power-supply monitor. The monitor can also reset the audio side. ^[raw/docs/taito-f3/list.txt]
- Traced nets: MB3771 pin 8 (`~RESET`) → R57 pull-up, pal5.23, fio.115, pal1.14; FIO pin 94 (main CPU reset) → cpu.6, cart A80, fcm.133, fdp.206, fda.89, pal8.1. ^[raw/docs/taito-f3/list.txt]
- pal1, pal5 pin names for the reset net are `?`-marked in the notes. ^[raw/docs/taito-f3/pal1.txt] ^[raw/docs/taito-f3/pals.txt]
- Software-side, MAME models a watchdog reset via a write to the first FIO register offset (`0x4a0000`); see [[hardware/memory-map]] for the address. ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Hypothesis: the F3 runtime need only model the watchdog as a write to 0x4a0000; whether the hardware timeout matches MAME's `WATCHDOG_TIMER` default is not covered by these notes.

## CPU/FDP access gating (ic9, ic64, ic65)

ic9 gates 1–2 compute "cpu.dsack0 && cpu.dsack1 && cpu.as → fdp.2" (all active low, so the output is low if any of the three is asserted), and gates 3–4 buffer fdp.116→fdp.113 and fdp.112→fdp.117. ^[raw/docs/taito-f3/ic9.txt] The ic64/ic65 flip-flop circuit is described in [[hardware/fcm]].

## MAME's view of the board

MAME's PCB diagram comment lists D29-/D49-/D53-series PALs, the 68EC020, FIO, 93C46, MB3771 (3771), TC51832 SRAM, HM511664, 5701, 5505-OTIS and 5510-ESP, matching the notes' inventory. ^[raw/emu-source/mame-0.289/taito_f3.cpp]
