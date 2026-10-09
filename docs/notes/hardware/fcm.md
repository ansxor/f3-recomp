---
title: TC0660FCM (FCM) and the AS/DSACK gating
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [pcb, memory-map, interrupt, cpu, io]
sources: [raw/docs/taito-f3/fcm.txt, raw/docs/taito-f3/website/fcm.html, raw/docs/taito-f3/list.txt, raw/docs/taito-f3/ic9.txt, raw/docs/taito-f3/address-mapping.txt, raw/docs/taito-f3/pal1.txt, raw/docs/taito-f3/pal2.txt]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/fcm.txt, note: "FCM pin-by-pin trace (pins marked '?' on the html page are inferred from PCB traces and die photos)"}
  - {kind: doc, ref: raw/docs/taito-f3/list.txt, note: "ic64/ic65 netlist and the notes' reasoning about fcm.109"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fcm.html, note: "pinout table with i/o/nc direction guesses"}
contradictions:
  - "fcm.txt labels pins 119/128 'cart.C04 (clock?)' and fcm.html labels them 'fdp.118 (vsync)'; the cartridge page says C04, fdp.118, fcm.119, fcm.128 and pal7.3 are one net, so these are not a real conflict."
  - "list.txt connects fcm.109 to ic64.9 and fcm.html says 'ic64.9' as an input; the notes' text says the FCM 'perhaps' answers with DSACK once it sees this signal go low, which is a guess, not an observed behaviour."
supersedes: []
---

# TC0660FCM (FCM)

The TC0660FCM is a 160-pin QFP custom chip (40×40 mm, 0.65 mm pitch). 12Me21's page says it "seems to be responsible for various miscellaneous functions": CPU address mapping, communication between the CPU and the FDP, generating CPU interrupts, address buffering for the ESP's RAM, and so on. ^[raw/docs/taito-f3/website/fcm.html] ^[raw/docs/taito-f3/chips.txt] It is the board's "everything glue" chip. For the main bus layout it serves see [[hardware/memory-map]]; the PALs in front of it are in [[hardware/pal-decoders]].

Pin directions come from PCB traces and die photos (cell markers `i?`, `o?`, `io?`, `nc?` on the html page mean "probably"). ^[raw/docs/taito-f3/website/pinout.txt] ^[raw/docs/taito-f3/website/fcm.html]

## Pin groups (summary; full list in `raw/docs/taito-f3/fcm.txt`)

| Pins | Signal | Notes |
|---|---|---|
| 2–5, 144–149, 151–156 | cpu.d16..d31 (data, shown as "16-bit" in the html) | the FCM sees only the upper half of the bus ^[raw/docs/taito-f3/fcm.txt] ^[raw/docs/taito-f3/website/fcm.html] |
| 6–9, 11 | cpu.a19..a16, a15 | "address high" ^[raw/docs/taito-f3/fcm.txt] |
| 12–19 | cpu.a7..a0 | "address low" ^[raw/docs/taito-f3/fcm.txt] |
| 84–89, 91–96 | fdp.29..17 area | CPU address passed to the FDP ("CPU address to FDP?") ^[raw/docs/taito-f3/fcm.txt] |
| 111–114 | fdp.16, 14, 13, 12 | CPU address to the FDP ^[raw/docs/taito-f3/fcm.txt] |
| 97–99 | fda.27, 28, 26 | "FDA something" ^[raw/docs/taito-f3/website/fcm.html] |
| 102–105 | work-RAM chip selects (ic14–ic17 pin 20) | one per byte lane ^[raw/docs/taito-f3/fcm.txt] |
| 106 | fio.118 | chip select? ^[raw/docs/taito-f3/fcm.txt] |
| 107, 108 | cpu.dsack0, dsack1 | outputs ^[raw/docs/taito-f3/fcm.txt] |
| 109 | ic64.9 | gated "AS" input, see below ^[raw/docs/taito-f3/fcm.txt] |
| 115–117 | cpu.ipl2, ipl1, ipl0 | interrupt level outputs ^[raw/docs/taito-f3/fcm.txt] |
| 119, 128 | fdp.118 / cart C04 (vsync) | vsync input ^[raw/docs/taito-f3/fcm.txt] ^[raw/docs/taito-f3/website/fcm.html] |
| 125 | cpu.avec | autovector output ^[raw/docs/taito-f3/fcm.txt] |
| 126 | pal2.22 | interrupt-acknowledge decode (FC=111) ^[raw/docs/taito-f3/pal2.txt] ^[raw/docs/taito-f3/website/fcm.html] |
| 127 | cart A90 | cartridge pin ^[raw/docs/taito-f3/fcm.txt] |
| 131 | 30.47618 MHz clock input | ^[raw/docs/taito-f3/fcm.txt] |
| 133–138 | cpu.reset, siz0, siz1, R/W, DS, AS | ^[raw/docs/taito-f3/fcm.txt] |
| 141, 142, 143 | pal1.22, pal1.20, pal1.19 | address-range selects for `0xC-F`, `0x6`, `0x4` ^[raw/docs/taito-f3/fcm.txt] ^[raw/docs/taito-f3/pal1.txt] |
| 48, 122 | otis.e | OTIS bus-cycle signal ^[raw/docs/taito-f3/fcm.txt] |
| 49, 51, 53, 55, 123 | otis serial bclk, ser1–3, serlr | audio serial lines ^[raw/docs/taito-f3/fcm.txt] |
| 52, 54, 56, 57 | esp.ser0–3 | ^[raw/docs/taito-f3/fcm.txt] |
| 58 | dac.3 (data) | audio serial data out ^[raw/docs/taito-f3/fcm.txt] |
| 62–78 (even/odd) | ESP RAM address (a0..a7)/data (d9..d16) | data/address latch for the ESP's RAM ^[raw/docs/taito-f3/fcm.txt] |
| 79 | esp.5 (gate) | ^[raw/docs/taito-f3/fcm.txt] |

So the FCM also carries the OTIS/ESP serial audio stream to the DAC and buffers ESP RAM addressing; these audio-bus pins are covered more fully on [[hardware/sound]]. ^[raw/docs/taito-f3/website/fcm.html]

## What the FCM decodes

- The FCM sees only cpu address bits A0–A7 and A15–A19; A20–A23 are decoded by pal1/pal2 and handed over as range selects (pins 141–143), with the FCM doing the finer decode. FDP sees A0–A14, FIO A0–A4, pal8 A8. ^[raw/docs/taito-f3/address-mapping.txt]
- `0x4?????` (pal1.19 → fcm.143): the notes say the FCM handles work RAM (`0x400000`), palette RAM (`0x440000`, "may be mirrored more"), FIO registers (`0x4a0000`) and the timer interrupt register (`0x4c0000`), "entirely" it is assumed. ^[raw/docs/taito-f3/address-mapping.txt]
- `0x6?????` (pal1.20 → fcm.142 and fdp.1): graphics RAM and FDP control registers; "graphics ram: handled by fcm". ^[raw/docs/taito-f3/address-mapping.txt]
- `0xC-F` (pal1.22 → fcm.141): "not sure why tbh"; pal2 handles DSACK and chip selects for `0xC?????`. ^[raw/docs/taito-f3/address-mapping.txt] (pal1's printed o22 equation is questioned in [[hardware/pal-decoders]].)
- DSACK for the program-ROM space: "not sure who sets DSACK. may be FCM?" ^[raw/docs/taito-f3/address-mapping.txt]
- Interrupts: FCM drives IPL2–0 and AVEC (pins 115–117, 125) and takes vsync on pins 119/128 and the interrupt-acknowledge decode from pal2 on pin 126 (FC=111). Interrupt levels and timing are in [[hardware/interrupts]]. ^[raw/docs/taito-f3/fcm.txt] ^[raw/docs/taito-f3/pal2.txt]

## AS / DSACK gating with ic64 and ic65 ("whatever this fuckin thing is")

ic64 is a 74F74 (two D flip-flops) and ic65 an SN74F04 hex inverter (only two inverters are used). The notes believe they are "probably related to memory access between the FDP and main cpu". ^[raw/docs/taito-f3/list.txt]

Nets: ^[raw/docs/taito-f3/list.txt]

| Net | Path |
|---|---|
| CPU `AS` | cpu.as → ic65.11, fcm.138, pal2.7 |
| inverted AS | ic65.10 → ic64.4 and ic64.10 (both presets) |
| "idk fdp 205" | fdp.205 → ic65.13; inverted via ic65.12 → ic64.3 (clock 1) |
| graphics RAM chip enable | fdp.32 → ic64.11 (clock 2), ic11.22, ic12.22 |
| FF1 output → FF2 input | ic64.5 → ic64.12 |
| FF2 output | ic64.9 → fcm.109 |

Behaviour as derived by the notes (not measured end to end): when AS is asserted the presets are released; ic64 FF1's D input is low, so when fdp.205 goes low (clock 1 rising) Q1 goes low; if a pulse then arrives on clock 2 (graphics RAM chip enable going high, i.e. disabled) before AS next rises, Q2 goes low. In order, fcm.109 goes low only after (1) fdp.205 goes low and (2) the graphics-RAM chip enable goes high; whenever the CPU stops presenting an address (AS negated) the state resets and fcm.109 returns high. ^[raw/docs/taito-f3/list.txt]

Notes' guesses: this "results in" the FCM asserting DSACK to let the CPU continue its bus cycle once the FDP is "ready"; on the single-board F3, fdp.205 may have been wired straight to fcm.109 and the later boards added the extra "graphics RAM disabled" condition because the direct connection was unreliable. ^[raw/docs/taito-f3/list.txt] Hypothesis for the runtime: CPU accesses to FDP-owned memory (`0x6?????`) are held until the FDP finishes its own RAM cycle, so CPU wait states there are variable — no cycle numbers are given in these files. ^[raw/docs/taito-f3/list.txt]

ic9 (SN74LS08N, four AND gates) adds a related path: gates 1–2 compute "cpu.dsack0 && cpu.dsack1 && cpu.as" (all active low, so true if any of the three is asserted) and output it to fdp.2; gates 3 and 4 buffer fdp.116 → fdp.113 and fdp.112 → fdp.117. ^[raw/docs/taito-f3/ic9.txt] See [[hardware/fdp]].
