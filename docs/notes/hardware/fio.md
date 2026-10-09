---
title: TC0640FIO I/O chip (inputs, coins, EEPROM, watchdog)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [io, input, reset, pcb, memory-map]
sources:
  - raw/docs/taito-f3/website/fio.html
  - raw/docs/taito-f3/fio.txt
  - raw/docs/taito-f3/fio-mem.txt
  - raw/docs/taito-f3/fio-mem-old.txt
  - raw/docs/taito-f3/port.txt
  - raw/docs/taito-f3/list.txt
  - raw/emu-source/mame-0.289/taito_f3.cpp
games: []
addresses: [0x004A0000, 0x004A0004, 0x004A0010, 0x004A0014]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fio.html, note: "register map $00-$1F, pinout, rotary-encoder counters"}
  - {kind: doc, ref: raw/docs/taito-f3/fio-mem.txt, note: "values read at 0x4a0000 with each input pressed"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3.cpp, note: "f3_control_r/w and the f3 input ports"}
contradictions:
  - "Player 1/2 button 4: MAME's IN.0 places them at low-word bits 3 and 7 (0x08, 0x80); fio-mem.txt/fio.html show them on port $00 bits 0 and 1 (the 0x4a0000 word's high byte) and pins 34/39 (which would be bits 3 and 7 of port $03) as unused."
  - "Watchdog: MAME resets the watchdog on any write to the first dword (0x4a0000..0x4a0003); fio.html says 'which address do you have to write to? MAME just checks for any writes to the first 4 bytes, but this is probably wrong.'"
  - "fio.txt annotates pins with 'iN.M' dword/bit guesses that are marked 'idk'; fio.html's port numbers ($00-$1F) are the later, tested naming."
supersedes: []
---

# TC0640FIO I/O chip

The F3's TC0640 "FIO" handles buttons, joysticks, coins, service/test switches, the 93C46 EEPROM, coin counters/lockouts, rotary-encoder counters and the watchdog. It is a 120-pin chip on the main board; this page keeps the software-visible register map and summarises the wiring. The full pinout is in `raw/docs/taito-f3/website/fio.html` and `raw/docs/taito-f3/fio.txt`.^[raw/docs/taito-f3/website/fio.html] ^[raw/docs/taito-f3/fio.txt]

Bus: main CPU range `0x004A0000..0x004A001F` (32 bytes, `A0`-`A4` are chip inputs); MAME maps `0x4a0000-0x4a001f` to `f3_control_r/w` with 32-bit access, reads allowed for dword offsets 0-5 only.^[raw/docs/taito-f3/website/fio.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp] See [[hardware/memory-map]] for the surrounding map.

## Register map (12Me21's `$00-$1F`, byte offsets from `0x004A0000`)

Labels follow F3 usage because that is what was tested; all buttons/switches are active low (pressed = 0).^[raw/docs/taito-f3/website/fio.html] ^[raw/docs/taito-f3/fio-mem.txt]

| Port | Dir | Bits 7..0 |
|---|---|---|
| `$00` | in | cart pin B4?, B3?, B2?, B1?, test (jamma), 1p button 5*, 2p button 4, 1p button 4 |
| `$01` | in | coin 4, coin 3, coin 2, coin 1, (unused), cart pin B5?, test (board), EEPROM DO |
| `$02` | in | 4p start, 3p start, 2p start, 1p start, service (4p), service (3p), service (jamma), tilt |
| `$03` | i/o | (unused)?, 2p btn 3, 2p btn 2, 2p btn 1, (unused)?, 1p btn 3, 1p btn 2, 1p btn 1 |
| `$04` | out | bits 3..0: coin 2 counter, coin 1 counter, coin 2 lockout, coin 1 lockout |
| `$06` | ctrl | direction of port `$03` (0 = input, 1 = output) |
| `$07` | in | 2p right/left/down/up (bits 7..4), 1p right/left/down/up (bits 3..0), or dial inputs |
| `$08-$0F` | in | 16-bit (little-endian) counters: 1p up/down (`$08`), 1p left/right (`$0A`), 2p up/down (`$0C`), 2p left/right (`$0E`) |
| `$12` | in | 4p btn 3/2/1, (unused), 3p btn 3/2/1, (unused) |
| `$13` | i/o | bits: `Z4`, `Z3` (pins 59/58, bits 1,0), EEPROM DI (bit 2), CLK (bit 3), CS (bit 4), bits 5-7 (pins 65-67, unused) |
| `$14` | out | 4-player coin counters 4/3 and lockouts 4/3, same bit order as `$04` |
| `$16` | ctrl | direction of port `$13` |
| `$17` | in | 4p right/left/down/up (bits 7..4), 3p right/left/down/up (bits 3..0), or dials |
| `$18-$1F` | in | counters: 3p up/down (`$18`), 3p left/right (`$1A`), 4p up/down (`$1C`), 4p left/right (`$1E`) |

`$05`, `$10`, `$11`, `$15` are marked `?` (unknown) in the notes; pin labels with `?` (cartridge pins B1-B5, "unused" pins 34/39/44/48, `1p button 5*`) are the notes' own uncertainty.^[raw/docs/taito-f3/website/fio.html]

- Rotary encoders: each of the 8 joystick axes can drive a 16-bit counter that increments/decrements one per step (a full cycle 00 01 11 10 = 4 steps); counters are always on, the game chooses whether to read the joystick bits or the counters. F3 games are said to use only two dials (1p and 2p left/right), though all 8 axes were tested.^[raw/docs/taito-f3/website/fio.html]
- The JP "joystick/sensor" jumper only sets pull-up/down resistors on some input lines.^[raw/docs/taito-f3/website/fio.html]

### What a CPU read of `0x004A0000` returns (fio-mem.txt)

`fio-mem.txt` lists the 16-bit values read at word offsets 0-F (`0x4a0000 + 2*n`) while pressing inputs. Idle values include word0 `FFFE`, word1 `FFFF`, word3 `00FF`, word9 `FF08`, wordB `0FFF`; dial words show whatever the counter holds.^[raw/docs/taito-f3/fio-mem.txt]

- Word 0 = (port `$00` high byte, port `$01` low byte): board test pulls `FFFC` (port `$01` bit 1), jamma test `F7FE` (port `$00` bit 3), 1p btn 4 `FEFE`, 2p btn 4 `FDFE`, coin 3 `FFBE`, coin 4 `FF7E`.^[raw/docs/taito-f3/fio-mem.txt]
- Word 1 = ports `$02`,`$03`: 1p start `EFFF`, 2p start `DFFF`, 3p start `BFFF`, 4p start `7FFF`, tilt `FEFF`, service `FDFF` / `FBFF` / `F7FF` (jamma/3p/4p), 1p btn 1/2/3 `FFFE/FFFD/FFFB`, 2p btn 1/2/3 `FFEF/FFDF/FFBF`.^[raw/docs/taito-f3/fio-mem.txt]
- Word 3 = joysticks 1p/2p in the low byte (`00FE` up, `00FD` down, `00FB` left, `00F7` right for 1p; `EF/DF/BF/7F` for 2p); the high byte is `$06` (direction, reads `00`).^[raw/docs/taito-f3/fio-mem.txt]
- Word 9 = 3p/4p buttons in the high byte (`FE08` 3p btn 1 … `BF08` 4p btn 3), low byte `08` at idle (port `$13`); wordB = 3p/4p joysticks in the low byte, high byte `0F` (direction register `$16`).^[raw/docs/taito-f3/fio-mem.txt]
- Hypothesis (the wiki's reading of those values; `fio-mem.txt` gives only word index and value): word n holds ports 2n and 2n+1, with port 2n in the high byte. It is unexplained how a 16-bit read sees two ports when the chip's data pins are D24-D31; the notes do not address this.^[raw/docs/taito-f3/website/fio.html] ^[raw/docs/taito-f3/fio-mem.txt]
- `fio-mem-old.txt` is an earlier capture of the same words (partial; word E/F values differ).^[raw/docs/taito-f3/fio-mem-old.txt]
- "0300 at startup sometimes?" is noted for word 2.^[raw/docs/taito-f3/fio-mem.txt]

## MAME's register model (32-bit dword offsets)

| Dword offset (address) | Read | Write |
|---|---|---|
| 0 (`0x4a0000`) | IN.0: low 16 bits: buttons 1-4 for P1/P2 (bits 0-7, active low), tilt, services, starts (bits 8-15); bits 16-31: EEPROM-read byte (DO in bit 0, service/test 0x02, "another service mode" 0x08, coins 1-4 in 0x10-0x80) mirrored in both bytes | any write: watchdog reset |
| 1 (`0x4a0004`) | IN.1: low byte P1/P2 joysticks; bits 8-15 read 1; bits 16-31: last coin counter/lockout word | bits 24-27: lockout 1/2 (active low), counter 1/2 |
| 2, 3 (`0x4a0008`, `0x4a000c`) | analog dial 0/1 (12-bit value rearranged as `(d&0xf)<<12 | (d&0xff0)>>4` in the low 16 bits) | - |
| 4 (`0x4a0010`) | IN.4: P3/P4 buttons (bits 8-15) | low byte → EEPROM: bit 2 DI, bit 3 CLK, bit 4 CS |
| 5 (`0x4a0014`) | IN.5: P3/P4 joysticks (low byte), last coin word in 16-31 | bits 24-27: lockout 3/4 (active low), counter 3/4 |

^[raw/emu-source/mame-0.289/taito_f3.cpp]

- Offsets ≥ 6 read `0xffffffff` and log an "unmapped control address" warning; writes to other offsets are logged.^[raw/emu-source/mame-0.289/taito_f3.cpp]
- The coin counter/lockout byte in MAME corresponds to port `$04` (byte 4, top byte of dword 1) and `$14` (byte 0x14, top byte of dword 5); the EEPROM byte corresponds to port `$13` (low byte of dword 4). Hypothesis; the match is by bit order, not stated in either source.^[raw/docs/taito-f3/website/fio.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- `fio.txt` has the same (brief) MAME comparison and the plain list of the 4a0000 block layout: dword 0 = watchdog reset, 1 = player 1/2 coin, 4 = EEPROM, 5 = player 3/4 coin.^[raw/docs/taito-f3/fio.txt]
- MAME's header lists `TODO: Use TC0640FIO device implementation from taito/taitoio.cpp`.^[raw/emu-source/mame-0.289/taito_f3.cpp]

## EEPROM

- EEPROM (93C46, 16-bit organisation in MAME) pins: CS port `$13` bit 4 (FIO pin 64), CLK bit 3 (pin 63), DI bit 2 (pin 62), DO to port `$01` bit 0 (pin 82). Pins 62-64 have pull-downs, 65-67 pull-ups (unused, nc on single board).^[raw/docs/taito-f3/website/fio.html] ^[raw/docs/taito-f3/fio.txt] ^[raw/emu-source/mame-0.289/taito_f3.cpp]

## Reset / watchdog / clocks

- Pin 94 is the chip's RESET output ("Watchdog Output") that drives the reset inputs of most chips including the main CPU; pin 95 is HALT. Pin 115 is a separate RESET input from the power-supply monitor (MB3771). The main CPU reset is generated by the FIO in response to the watchdog or the power supply monitor; the monitor can also reset the audio side.^[raw/docs/taito-f3/website/fio.html] ^[raw/docs/taito-f3/list.txt]
- Which address clears the watchdog is unknown (see contradictions); MAME uses any write to `0x4a0000..0x4a0003`. The watchdog timeout length is not documented in the notes.^[raw/docs/taito-f3/website/fio.html] ^[raw/emu-source/mame-0.289/taito_f3.cpp]
- Pin 107 takes a 1 MHz clock (the same net as DUART pin 36), pins 109-113 are `A4`-`A0`, 116 R/W, 117 OE, 118 chip select from FCM pin 106 (see [[hardware/fcm]]). Pins 2-4 are tied to VCC/GND (probably hardwired settings).^[raw/docs/taito-f3/website/fio.html] ^[raw/docs/taito-f3/fio.txt]
- Coin mech drivers: 1p/2p outputs are on FIO pins 6-9 (lockout 1/2, counter 1/2), 3p/4p on pins 69-72 through a driver IC34.^[raw/docs/taito-f3/fio.txt] ^[raw/docs/taito-f3/list.txt]
- `port.txt` maps the 3P/4P/AA input connectors to the ports and FIO pins (e.g. the 3p/4p buttons through ICs 55-58 to FIO 41-56); see that file for the wiring.^[raw/docs/taito-f3/port.txt]

## Related

- Sound-CPU reset line is separate (0xc80000/0xc80100): [[hardware/sound]].
- Board overview: [[hardware/board]].
