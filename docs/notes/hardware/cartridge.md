---
title: F3 cartridge connector (ports A–D)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [pcb, memory-map, audio, sprite, tilemap]
sources: [raw/docs/taito-f3/cart.txt, raw/docs/taito-f3/cart2.txt, raw/docs/taito-f3/cart3.txt, raw/docs/taito-f3/website/cart.html, raw/docs/taito-f3/pal15.txt, raw/docs/taito-f3/pal9.txt, raw/docs/taito-f3/pal12.txt, raw/docs/taito-f3/website/memory layout.html]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/cart.txt, note: "per-pin trace of connector A/B/C/D to board and cartridge parts"}
  - {kind: doc, ref: raw/docs/taito-f3/website/cart.html, note: "tidy table of the same connector with clock figures"}
contradictions:
  - "cart.txt and cart.html put cpu.a0/a1 on A06/A07 (not connected on the cart) and cpu.d6/d7 on B06/B07; cart2.txt and cart3.txt (earlier drafts) list A06/A07 as cpu.d6/d7 and cart3.txt lists B06/B07 as '?'."
  - "cart.txt's C29 maps to fdp.152 while cart3.txt's C29 maps to fdp.153; cart.txt also notes FDP152 as GND in its FDP summary (notes disagree with each other)."
supersedes: []
---

# F3 cartridge connector (ports A–D)

The F3 cartridge plugs into four 96-pin edge connectors, A, B, C and D (rows of 32 pins, numbered 1–96 in the notes). The main CPU bus, the OTIS sample bus, the FDP graphics ROM bus and the audio CPU's ROM bus each cross a different port. This page summarises what crosses; the pin-by-pin tables are in `raw/docs/taito-f3/cart.txt` and `website/cart.html`. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/website/cart.html]

Keying: cart.txt records region key positions: x33–35 key (Japan), x36–38 "key? (america?)", x39–41 key (Europe), and x56–58 Europe, x59–61 "america?", x62–64 Japan. ^[raw/docs/taito-f3/cart.txt]

## Port A — main CPU bus (program ROM)

- A1–A5 GND; A6–A27 are the CPU address bus a0..a21 (22 pins); A28–A32 VCC. A65–A66 are a22/a23. ^[raw/docs/taito-f3/website/cart.html] ^[raw/docs/taito-f3/cart.txt]
- cpu.a0 and cpu.a1 (A06, A07) are "nc" on the cartridge; program ROM A0..A16 hang off cpu.a2..a18 (A08–A24), i.e. the ROMs are addressed in 32-bit units. pal15 inputs come from a17–a21 plus a22/a23, AS, DS, R/W, FC0–2. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/pal15.txt]
- Control signals on A67–A88: AS, DS, R/W, DSACK0/1, IPL0–2, FC0–2, SIZ0/1, RESET, HALT, clock (A82, shielded, the 15.238 MHz net), BR, BG, BERR, RMC, AVEC, CDIS. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/website/cart.html] See [[hardware/clocks]].
- A89 (also tied to C03) connects to fdp.180 and fdp.207 and carries the 3.33575 MHz clock; A90 goes to fcm.127. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/website/cart.html] See [[hardware/fcm]].
- A91–A96 carry data d0–d5. ^[raw/docs/taito-f3/cart.txt]
- Program-ROM output enable and the upper address bits come from pal15 (cartridge), which decodes AS with FC=?01/?10 at addresses `00*`; the ROM's A17/A18 are cpu.a19/a20. ^[raw/docs/taito-f3/pal15.txt] See [[hardware/pal-decoders]].
- Because pal15 does not look at cpu.a21, the same ROM data appears again 0x200000 higher inside the `00*` decode (hypothesis: derived from the equation; memory.html itself says "size and mapping of program rom depends on the cartridge"). ^[raw/docs/taito-f3/pal15.txt] ^[raw/docs/taito-f3/website/memory.html]

## Port B — main CPU data bus d6..d31, FIO extra inputs, OTIS sample bus

- B1–B5 are FIO inputs fio.86/85/84/83/80, "could be used for extra buttons" per cart.html. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/website/cart.html]
- B6–B27 carry cpu.d6..d27, B65–B68 carry d28..d31 (four program ROM chips, one per byte lane; prg0..prg3). ^[raw/docs/taito-f3/cart.txt]
- B69–B84 are the OTIS multiplexed address/data bus (a4..a19 / d0..d15): the sample ROM address is latched into 74F373s on the cartridge (ic34–ic36 in cart.txt numbering) and the same pins return the sample ROM data. B85–B88 are OTIS a0..a3. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/cart3.txt]
- B89–B92 are the expanded bank-select bits (a20..a23) from the ic30 dual-port "voice bank list" on the board. B93–B96 are OTIS E, BS, RAS (also latch enable of the three address latches) and CAS. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/draw.txt] See [[hardware/sound]].
- Hypothesis: the bank counter ic21/ic22 and ic30 serve as the "OTIS bank expander" shown on the schematics page; the sample ROM address space therefore exceeds OTIS's native 20-bit reach. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/website/schematics.html]

## Port C — graphics (FDP ROM buses)

- C1 is the 13.343 MHz clock, C2 the 6.6715 MHz clock, C3 the 3.33575 MHz clock; C4 is connected to fdp.118 (vsync), fcm.119/128 and pal7.3. ^[raw/docs/taito-f3/website/cart.html] ^[raw/docs/taito-f3/cart.txt]
- C5–C32 and C69–C88 connect to FDP pins 125–179 (the tracing notes pair FDP118 with C4, FDP125-156 with C5-32, and FDP157-168 with C65 onward). The bus is multiplexed: address out and data in share the same pins, with a multi-step cycle sequenced by pal9. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/pal9.txt]
- The sprite and tile ROMs are addressed through latches (74F373) and read back through buffers (74F244) on the cartridge; tile ROMs use 16-bit mode with an address space one quarter as large (two fewer bits) and half as many chips as sprite ROMs. pal9/pal10 supply latch-enable, output-enable and byte-mode controls, and the sprite ROM OE groups (pal9.15–18 = groups 3..0). ^[raw/docs/taito-f3/pal9.txt] See [[hardware/pal-decoders]] and [[hardware/sprites]].
- Hypothesis on size: pal9 latches graphics address a20..a23 (C25–C28) and pal10 notes tiles only use a 23-bit address. ^[raw/docs/taito-f3/pal9.txt]
- C89–C92 and D1–D13 carry the audio CPU's address (a1..a17) to the audio program ROM (C89–C92 → audio a1..a4; D1 → a5, and so on), with a0 unconnected. ^[raw/docs/taito-f3/cart.txt]
- C74/C75 do not connect to anything per cart.html. ^[raw/docs/taito-f3/website/cart.html]

## Port D — audio CPU bus

- D1–D13 audio a5..a17 to the audio program ROMs ("rom13/rom14", a 27C4001 pair, one per byte lane); D14–D19 audio a18–a23, which pal12 decodes into ROM A17/OE (`o12` A18 is unused on this board). D20–D23 are AS, LDS, UDS, R/W; D28–D30 FC0–2 (D28 "BROKEN" on the traced cartridge); D31 reset. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/pal12.txt]
- D77–D92 are audio data d0..d15; several (D83, D84, D90–D92, D17, D19) are marked BROKEN in the traced cartridge (damage on the board sampled, not a design feature). ^[raw/docs/taito-f3/cart.txt]
- D69 is the same clock as A82; D70–D76 and the control lines BGACK/BG/MODE/BERR/AVEC are nc/unused. ^[raw/docs/taito-f3/cart.txt] ^[raw/docs/taito-f3/website/cart.html]

## Software-visible consequence

- The Kirameki cartridge detects writes near address `0x300000` and switches audio-CPU program ROM banks; the memory layout page labels `30xxxx` "kirameki bank switch". MAME maps `0x300000-0x30007f` as a write handler for the same purpose. ^[raw/docs/taito-f3/website/memory.html] ^[raw/docs/taito-f3/website/memory layout.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] See [[hardware/memory-map]].
