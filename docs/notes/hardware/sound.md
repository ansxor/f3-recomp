---
title: Sound board section (68000, ES5505 OTIS, ES5510 ESP, MC68681 DUART)
created: 2026-10-09
updated: 2026-10-15
type: hardware
tags: [audio, cpu, memory-map, reset, pcb]
sources:
  - raw/docs/taito-f3/sound-address.txt
  - raw/docs/taito-f3/website/sound/memory.html
  - raw/docs/taito-f3/audiocpu.txt
  - raw/docs/taito-f3/duart.txt
  - raw/docs/taito-f3/otis-bank.txt
  - raw/docs/taito-f3/list.txt
  - raw/docs/taito-f3/5505-5510.txt
  - raw/docs/taito-f3/clocks.txt
  - raw/emu-source/mame-0.289/taito_en.cpp
  - raw/emu-source/mame-0.289/taito_f3.cpp
  - raw/docs/otis-esp-dac-routing-2026-10-09.txt
  - raw/docs/otis-esp-dac-routing-2026-10-09-followup.txt
  - raw/tests/2026-10-10-otis-esp-routing-experiments.md
  - raw/tests/2026-10-10-enhanced-effects-hardware-routing.md
  - raw/docs/runtime-audio-es5505.md
  - raw/docs/runtime-audio-es5510.md
  - raw/docs/runtime-audio-duart-and-gain.md
  - raw/docs/runtime-audio-mailbox.md
games: []
addresses: [0x00C00000, 0x00C80000, 0x00C80100, 0x00300000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/sound-address.txt, note: "sound CPU address decode"}
  - {kind: doc, ref: raw/docs/taito-f3/otis-bank.txt, note: "bank counter/flip-flop wiring (ic21, ic22, ic30)"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_en.cpp, note: "sound 68000 map, clocks, DUART I/O, OTIS banking"}
  - {kind: doc, ref: raw/docs/otis-esp-dac-routing-2026-10-09.txt, note: "12's initial board-routing relay (with reversed ESP serio0..3 numbering); historical context"}
  - {kind: test, ref: raw/tests/2026-10-10-otis-esp-routing-experiments.md, note: "runtime ESP microprogram instrumentation and OTIS pair muting/gain experiments"}
  - {kind: test, ref: raw/tests/2026-10-10-enhanced-effects-hardware-routing.md, note: "Enhanced hardware-routing test and 2400-frame Land Maker measurement"}
  - {kind: doc, ref: raw/docs/taito-f3/5505-5510.txt, note: "12's oscilloscope signal tracing from OTIS through FCM to ESP and DAC, Host Serial Control and SERMODE registers, FCM 74x174/74x373 replacement, and 128-BCLK LRCLK timing"}
  - {kind: doc, ref: raw/docs/taito-f3/clocks.txt, note: "audio BCLK 3.8095225 MHz and sample rate ~29.76189453 kHz (BCLK/128)"}
contradictions:
  - "ES5510 clock: list.txt shows an 8 MHz clock from counter ic18 (generator unknown); MAME uses 10 MHz (from Gun Buster schematics)."
  - "DUART OP6 -> ESPHALT in MAME; duart.txt marks all output pins OP0-OP7 as 'nc? unused'."
  - "Sound 68000 clock: notes say 15 MHz clock net (30.47618/2 elsewhere in the notes); MAME uses 30.47618 MHz / 2 for the sound CPU but 16 MHz for the main CPU."
  - "Sound RAM size/mirroring: notes' decode has 64 KiB repeating over 0x000000-0x07ffff and 0xf80000-0xffffff; MAME maps 64 KiB at 0x000000 mirrored to 0x03ffff, plus 0xff0000-0xffffff."
  - "ES5510 register window: notes decode 0x240000 (bits 17-16 don't care); MAME maps 0x260000-0x2601ff; same chip select, no real disagreement."
  - "otis-bank.txt says 'otis e = input divided by 8' for 3.8 MHz yet list.txt shows the 15 MHz clock on otis.34; 15.238/8 = 1.9 MHz, so either the OTIS input is the 30 MHz clock or the E ratio differs; the notes do not resolve this."
  - "Earlier 2026-10-09 relay (otis-esp-dac-routing-2026-10-09.txt) numbered ESP serio0..3 pins in reverse order, producing an apparent 1<->3 index swap; resolved by 12's oscilloscope signal tracing in 5505-5510.txt confirming OTIS ser1..3 -> ESP serio0..2 and ESP serio3 -> DAC."
supersedes: []
---

# Sound board section

The F3 audio side is a separate 68000 with an Ensoniq ES5505 ("OTIS") sample player, an ES5510 ("ESP") effects DSP, an MC68681 DUART used as timer/IRQ source, a dual-port RAM shared with the main CPU, and a volume chip. The main CPU can start/stop the audio CPU and exchange data through the shared RAM. Board-level information is in [[hardware/board]]; main-CPU clocks in [[hardware/clocks]].

## OTIS → ESP → DAC serial routing (confirmed)

Oscilloscope signal tracing by 12 (`5505-5510.txt`) confirms the physical serial audio routing between the ES5505 (OTIS), TC0660FCM, ES5510 (ESP), and TDA1543 DAC:

| Source | Destination | Status |
|---|---|---|
| OTIS channel 0 (`otis.6`, `ser0`) | unconnected (`nc`) | Confirmed by 12's oscilloscope signal tracing. ^[raw/docs/taito-f3/5505-5510.txt] |
| OTIS channel 1 (`otis.7`, `ser1`) | ESP channel 0 (`esp.15`, `serio0`) via FCM | Confirmed by 12's oscilloscope signal tracing. ^[raw/docs/taito-f3/5505-5510.txt] |
| OTIS channel 2 (`otis.8`, `ser2`) | ESP channel 1 (`esp.14`, `serio1`) via FCM | Confirmed by 12's oscilloscope signal tracing. ^[raw/docs/taito-f3/5505-5510.txt] |
| OTIS channel 3 (`otis.9`, `ser3`) | ESP channel 2 (`esp.13`, `serio2`) via FCM | Confirmed by 12's oscilloscope signal tracing. ^[raw/docs/taito-f3/5505-5510.txt] |
| ESP channel 3 (`esp.12`, `serio3`) | TDA1543 DAC (`dac.3`, data) via FCM | Confirmed by 12's oscilloscope signal tracing. ^[raw/docs/taito-f3/5505-5510.txt] |

In the earlier 2026-10-09 note (`otis-esp-dac-routing-2026-10-09.txt`), 12 had numbered ESP pins 12–15 in reverse order (`serio3..serio0` instead of `serio0..serio3`), producing an apparent 1↔3 index swap description (`OTIS ch 1 -> ESP ch 3, OTIS ch 3 -> ESP ch 1, ESP ch 0 -> DAC`). In `5505-5510.txt`, 12 corrected the pin-to-channel labels against the ES5510 datasheet (where pin 15 is `SERI/O0` and pin 12 is `SERI/O3`). The physical pin connections (`otis.7` → `esp.15`, `otis.8` → `esp.14`, `otis.9` → `esp.13`, `esp.12` → `dac.3`) are identical across both notes; `5505-5510.txt` confirms the corrected channel numbering. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/otis-esp-dac-routing-2026-10-09.txt]

In MAME pump and runtime HLE terms:
- OTIS Pair 1 (`buses 2/3`, `ser1`) feeds ESP channel 0 (`serio0`), which the sound ROM microprogram passes through directly as dry output.
- OTIS Pair 2 (`buses 4/5`, `ser2`) feeds ESP channel 1 (`serio1`), used for delay echo.
- OTIS Pair 3 (`buses 6/7`, `ser3`) feeds ESP channel 2 (`serio2`), used for reverb.
- OTIS Pair 0 (`buses 0/1`, `ser0`) is physically unconnected on the PCB (`nc`) and produces silence.
- ESP channel 3 (`serio3`) outputs serial stereo audio to the TDA1543 DAC.

MAME (`taito_en.cpp`) actually had Pairs 1, 2, 3 connected to the right ESP channels (`1->0, 2->1, 3->2`). MAME's actual routing bug was routing unconnected OTIS Pair 0 directly to the output mixer as an Aux bypass (plus applying MB87078 volume twice and not swapping L/R at the I2S DAC). The runtime's HLE audio (`runtime/audio/hle/effects.hpp`) routes Pair 1 (`buses 2/3`) to Dry (ESP ch 0), Pair 2 (`buses 4/5`) to Delay (ESP ch 1), Pair 3 (`buses 6/7`) to Reverb (ESP ch 2), and drops Pair 0 (`buses 0/1`). Reference retains MAME's baseline routing for comparison. ^[raw/emu-source/mame-0.289/taito_en.cpp L261-L269; raw/docs/taito-f3/5505-5510.txt]

`5505-5510.txt` further documents:
- **Register presets:** OTIS Host Serial Control register is set to `$63` `[01100011]` (Sony slave mode, SER3 output, SER0–2 inputs); ESP SERMODE register is set to `$07F8` `[0000011111111000]` ("normal"). ^[raw/docs/taito-f3/5505-5510.txt]
- **FCM logic replacement:** The TC0660FCM replaces a 74x174 4x D flip-flop for the serial audio channels (OTIS outputs connect to D, ESP inputs to Q, clocked by OTIS BCLK) and a 74x373 8x transparent latch buffering ESP RAM data/address lines (D inputs from ESP RA/D pins, Q outputs to RAM, GATE driving clock). ^[raw/docs/taito-f3/5505-5510.txt]
- **DAC timing & phase:** The serial protocol is "Sony" (similar to I2S). DATA transitions on the BCLK rising edge (half a cycle earlier than I2S); LRCLK transitions on the BCLK falling edge. The LRCLK frame length is 128 BCLK cycles (16 bits high = left sample, 112 bits low = right sample, with only the first 16 bits containing real data). Because the TDA1543 DAC expects standard I2S word select polarity (where low = left), internal left and right channels are swapped at the DAC output; tracing in `list.txt` indicates this swap persists through the analog filter/amp stages to the stereo connector. ^[raw/docs/taito-f3/5505-5510.txt; raw/docs/taito-f3/list.txt]

12's earlier remark regarding a "second 1↔3 index swap" arose from the preliminary reversed pin labeling of `serio3..serio0` in the 2026-10-09 relay; with the datasheet-aligned pin numbering in `5505-5510.txt`, no index swap exists between OTIS and ESP serial channels. Earlier analysis conjecturing reversed register↔pin numbering or predicting mix breakage is superseded by the hardware observations in `5505-5510.txt`. ^[raw/docs/otis-esp-routing-12-remark-2026-10-10.txt; raw/docs/taito-f3/5505-5510.txt]

## Runtime routing experiments

In commandw and landmakrj, runtime instrumentation found loaded ESP programs reading SER0, SER1, and SER2 and writing stereo output only to SER3. Pair muting changed output samples in the tested runtime; these results do not override the hardware-observed routing. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

HLE now implements the confirmed hardware wiring: Pair 1 (channels 2/3) → ESP ch 0 (Dry), Pair 2 (channels 4/5) → ESP ch 1 (Delay), Pair 3 (channels 6/7) → ESP ch 2 (Reverb), while Pair 0 (channels 0/1) is silent (unconnected on hardware). Reference remains unchanged and follows MAME. The routing behavior and 2400-frame Land Maker render are recorded in the runtime test note. ^[raw/tests/2026-10-10-enhanced-effects-hardware-routing.md]

Changing emulated OTIS→ESP gain from 0.18 to 1.0 raised Command War RMS by 5.55× in the test; whether physical hardware carries unity-gain words remains unestablished. ^[raw/tests/2026-10-10-otis-esp-routing-experiments.md]

See [[comparisons/hardware-vs-mame-audio-routing]] for the full comparison and [[hardware/board]] for chip inventory. ^[raw/docs/taito-f3/5505-5510.txt; raw/tests/2026-10-10-otis-esp-routing-experiments.md]

## Main-CPU view

| Address (main bus) | What |
|---|---|
| `0x00C00000-0x00C007FF` | MB8421 dual-port RAM (left side) shared with the sound CPU (MAME) |
| `0x00C80000` | write: releases the sound CPU from reset (`sound_reset_0_w` clears RESET) |
| `0x00C80100` | write: holds the sound CPU in reset (`sound_reset_1_w` asserts RESET) |
| `0x00300000-0x0030007F` | write: ES5505 ROM bank register for Kirameki only (`sound_bankswitch_w`, "unsupported game" log otherwise) |

^[raw/emu-source/mame-0.289/taito_f3.cpp]

- MAME starts the sound CPU held in reset in `machine_reset` ("qtheater relies on it (otherwise main CPU tries to reset it while 68k is working with irq table vectors)").^[raw/emu-source/mame-0.289/taito_f3.cpp]
- On Kirameki Star Road the bank writes select one of the 0x20000-byte ROM banks (`idx = (offset<<1)&0x1e (+1 for low-halfword accesses)`, minus 8 if ≥ 8) for the sound CPU's program-ROM windows; the MAME header thanks Acho A. Tang for this.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- The notes do not document the main-CPU side of the dual-port RAM or the reset latch; the addresses above are MAME's. ^[raw/emu-source/mame-0.289/taito_f3.cpp] Hypothesis: they are decoded by the FCM/PALs ([[hardware/fcm]], [[hardware/pal-decoders]]).

## Reset relationship

- The main CPU reset (active low) is generated by the FIO from its watchdog or the power-supply monitor MB3771; the monitor can also reset the audio side ("which i haven't examined much").^[raw/docs/taito-f3/list.txt]
- Sound 68000 `~RESET` (pin 21, pulled down via RB3) is annotated `otis.res pal8.18` (the OTIS reset net and PAL8 pin 18); the DUART's reset pin 34 connects to OTIS pin 22 (reset).^[raw/docs/taito-f3/audiocpu.txt] ^[raw/docs/taito-f3/duart.txt]
- See [[hardware/fio]] for the main-side reset/watchdog outputs.

## Sound CPU memory map (12Me21 `sound-address.txt`)

Address-bit patterns `a` real, `A` byte select, `.` don't care.^[raw/docs/taito-f3/sound-address.txt] ^[raw/docs/taito-f3/website/sound/memory.html]

| Start | Pattern | Region | Width |
|---|---|---|---|
| `$000000`/`$F80000` | `0000 0... aaaa aaaa aaaa aaaA` / `1111 1...` | work RAM (64 KiB, mirrored) | 16-bit |
| `$000000` | `0000 0?aa aaaa aaaa aaaa aaaA` | program ROM, supervisor program space only (vector fetch) | 16-bit |
| `$C00000` | `1100 0?aa aaaa aaaa aaaa aaaA` | program ROM (data and program, size depends on cartridge PAL) | 16-bit |
| `$140000` | `0001 01.. .... aaaa aaaa aaa0` | dual-port RAM | 8-bit even |
| `$200000` | `0010 00.. .... .... ...a aaaA` | OTIS (ES5505) registers | 16-bit |
| `$240000` | `0010 01.. .... ...a aaaa aaa1` | ESP (ES5510) registers | 8-bit odd |
| `$280000` | `0010 10.. .... .... ...a aaa1` | DUART | 8-bit odd |
| `$300000` | `0011 00.. .... .... ..aa aaa1` | OTIS voices' banks | 8-bit odd |
| `$340000` | `0011 01.. .... .... .... ..a0` | volume registers | 8-bit even |

- Program ROM mapping is done by the cartridge PAL; in pbobble4j (PAL D77-12) the two program-ROM rows above apply. At reset the ROM appears at address 0 in supervisor program space "to allow reading the pc reset vector at startup" and probably PALs 3-5 unmap RAM there. RAM is also mirrored to the top of the address space so it can be reached via signed 16-bit offsets from 0.^[raw/docs/taito-f3/sound-address.txt] ^[raw/docs/taito-f3/website/sound/memory.html]
- Accessing unmapped addresses freezes the CPU rather than raising a bus error (per the notes' wording; `~DTACK` has a pull-up, `~BERR` a pull-down).^[raw/docs/taito-f3/website/sound/memory.html] ^[raw/docs/taito-f3/audiocpu.txt]
- The ES5510 window decodes only some address bits (`[0010 01.. .... ...a aaaa aaa1]`, 8-bit odd); MAME places it at `$260000-$2601FF` (see contradictions). ^[raw/docs/taito-f3/sound-address.txt] ^[raw/emu-source/mame-0.289/taito_en.cpp]

### MAME map (`taito_en.cpp`)

- `0x000000-0x00ffff` RAM (mirror `0x30000`), `0xff0000-0xffffff` RAM mirror; `0x140000-0x140fff` dual-port RAM right side (high byte); `0x200000-0x20001f` ES5505; `0x260000-0x2601ff` ES5510 host (low byte); `0x280000-0x28001f` MC68681; `0x300000-0x30003f` ES5505 bank registers; `0x340000-0x340003` MB87078 volume (high byte, `data_w(offset ^ 1)`); `0xc00000-0xc7ffff` three ROM bank windows (`0xc00000-0xc1ffff`, `0xc20000-0xc3ffff`, `0xc40000-0xc7ffff`); `0xfffffd` read = DUART IRQ vector (CPU-space FC7 map).^[raw/emu-source/mame-0.289/taito_en.cpp]
- At reset the first 4 words of the audio-program region (16-bit index `0x80000..0x80003`, byte offset `0x100000`) are copied into RAM for the stack and reset vectors; bank windows are initialised to entries 0,1,2 (modulo bank count); ROM above `0x100000` is split into 0x20000-byte banks.^[raw/emu-source/mame-0.289/taito_en.cpp]
- The audio CPU is an `M68000` at 30.47618 MHz / 2 (≈15.238 MHz).^[raw/emu-source/mame-0.289/taito_en.cpp]
- MAME's TODO lists: ES5510 emulation imperfect; clocks and output channels verified only from Gun Buster schematics; "Where does the MB8421 go? Taito F3 (and Super Chase) have 2 of them on the sound area".^[raw/emu-source/mame-0.289/taito_en.cpp]

## ES5505 (OTIS) sample ROM banking

- Hardware (hypothesis, 12Me21's reading): a counter built from ic21 (74F74 flip-flops) and ic22 (74F161) divides OTIS's E clock (3.8 MHz) by 2, 4, 8 … 64 and drives the address pins `A0L`-`A4L` of dual-port RAM ic30 (MB8421), so each voice's bank bits are read out from ic30 as OTIS fetches sample data; the `BS` (bank-select) output of OTIS resets the counters. The notes guess "they set the 'bank' bit on the first voice only, and then use that to reset their counter when voice 0 is read".^[raw/docs/taito-f3/otis-bank.txt] ^[raw/docs/taito-f3/list.txt]
- CPU writes the per-voice bank values at `$300000` (odd bytes, `0011 00.. .... .... ..aa aaa1`).^[raw/docs/taito-f3/sound-address.txt]
- MAME: `en_es5505_bank_w` stores each 16-bit write into a table; the ROM address seen by the ES5505 is `((table[voice] & bankmask) << 20) + offset`, wrapped to the ROM size, where `bankmask = (OTIS ROM bytes / 0x200000) - 1`; there are 32 table entries (0x300000-0x30003f, word offsets) and the voice index comes from `get_voice_index()`.^[raw/emu-source/mame-0.289/taito_en.cpp]
- The ES5505 is clocked at 30.47618 MHz / 2 in MAME with 4 channels (8 routed outputs) into the ES5510 "pump"; pump sample rate 30.47618 MHz/(2·16·32).^[raw/emu-source/mame-0.289/taito_en.cpp]

## ES5510 (ESP)

- ESP clock: list.txt shows an 8 MHz net `ic18.2 → 5701.8 → esp.1` ("who generates this? 5701?") and a regulated ESP VDD via VR1/IC62.^[raw/docs/taito-f3/list.txt]
- MAME: `ES5510` at 10 MHz from Gun Buster schematics, `set_disable()`d (not emulated beyond host registers), host access at `0x260000` low byte; ESPHALT is driven by DUART output bit 6 (`duart_output`: bit 0x40 asserts halt).^[raw/emu-source/mame-0.289/taito_en.cpp]

## MC68681 DUART

- Clock pin 32 = 4 MHz; 1 MHz on pins 36/38 and 0.5 MHz on pins 2/39 from counter ic18 (div2 4 MHz, div8 1 MHz, div16 0.5 MHz of an 8 MHz source). IP0/IP1 tied to VCC (unused), outputs OP0-OP7 marked unused/nc in the notes; reset pin 34 to OTIS reset; pins 21/37 to PAL3, pin 26 to PAL8, pin 35 to PAL5; data/address lines to the sound 68000.^[raw/docs/taito-f3/duart.txt] ^[raw/docs/taito-f3/list.txt]
- The 1 MHz clock (pin 36) is also routed to FIO pin 107 ([[hardware/fio]]).^[raw/docs/taito-f3/list.txt]
- MAME: `MC68681` at 16 MHz / 4 = 4 MHz, serial clocks 16 MHz/2/8 and /16 (1 MHz, 0.5 MHz, 0.5 MHz, 1 MHz); IRQ → sound CPU level 6; interrupt vector fetched through the FC7 (interrupt acknowledge) space at `0xfffffd`; I/O pin assignment from Gun Buster schematics: IP0/IP1 = 5 V, IP2 = 1 MHz, IP3/IP4 = 0.5 MHz, IP5 = 1 MHz, OP6 = ESPHALT, others N/C.^[raw/emu-source/mame-0.289/taito_en.cpp]
- Hypothesis: the DUART's counter/timer inputs make it the sound CPU's periodic interrupt source (level 6); the notes do not say which sound-side interrupt the game software uses. ^[raw/emu-source/mame-0.289/taito_en.cpp] ^[raw/docs/taito-f3/duart.txt]

## Volume

- MB87078 electronic volume; byte writes at `$340000` (even bytes, notes) or `0x340000-0x340003` high byte (MAME, offset XOR 1 into `data_w`); gain writes with offset ≥ 2 scale ES5505 outputs and pump outputs by `data/32` in MAME's two-stage volume model (`MameRouting`). On hardware, the MB87078 provides post-DAC analog attenuation (`list.txt:265-280`); the runtime's default gain model is now `SingleStage` (single analog stage after ESP/DAC), while `MameRouting` is retained for MAME comparison.^[raw/docs/taito-f3/sound-address.txt] ^[raw/emu-source/mame-0.289/taito_en.cpp] ^[raw/docs/taito-f3/list.txt]

## Sound-CPU pinout

- `audiocpu.txt` lists the 68000's pins: `~DTACK` pulled up (R8), `~BGACK`, `~HALT`, `~RESET`, `~BERR`, `~IPL0-2` and `MODE` have pulldowns (RB3); `~AVEC` pin 23 listed without a source.^[raw/docs/taito-f3/audiocpu.txt]
- How the IPL lines are driven (including the DUART's IRQ → level 6) is not traced in the notes; see [[hardware/interrupts]] for the main CPU's side.

## Runtime-model detail pages

These pages describe how this runtime models each chip and how the Land Maker driver uses it. They are
MAME-derived model and ROM-analysis documentation, not board observations, and none of them overrides the
confirmed routing above: [[hardware/es5505-otis]] (registers, banks), [[hardware/es5510-esp]] (host
interface, instruction format, effects inventory), [[hardware/duart-and-volume]] (1 ms driver tick, ESP
halt, MB87078, master-volume timing), [[hardware/sound-mailbox]] (command ring and framing).
^[raw/docs/runtime-audio-es5505.md] ^[raw/docs/runtime-audio-es5510.md] ^[raw/docs/runtime-audio-duart-and-gain.md] ^[raw/docs/runtime-audio-mailbox.md]
