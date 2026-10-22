---
source_url: "Capstone script in the '## Capture script' section below (uv run python, ROMs from roms/kirameki)"
ingested: 2026-10-16
sha256: b040f1731bcd3746efb1f8fff6f68bc2606ada751579b76a79edd80712737382
game: kirameki
rom_sha256: d8ac9f4af1871e9d530407ac1181f5227b9d2badd6111c2198ff5103f19a4aa8
captured_with: "capstone 5.0.7 (git f47dde9)"
---
# Kirameki Star Road sound request, selector, and banking disassembly excerpts

Static disassembly excerpts captured from Kirameki Star Road (Japan, Ver 2.10J) main ROM
(`d8ac9f4af1871e9d530407ac1181f5227b9d2badd6111c2198ff5103f19a4aa8`)
and 3 MiB sound ROM (`e44-20.51` / `e44-21.52` interleaved 1 MiB program + `e44-15.53` word-swapped 2 MiB banked data).

## Main CPU sound request entry point ($0018e0)

Signature: `void sound_play(uint16_t sound_code)`. Callers push `sound_code` as a 16-bit word on the stack:
`move.w #code, -(a7); jsr $18e0.w; addq.l #2, a7`.
Stack argument `sound_code` is read at `$1c(a7)` (offset 28 = 24 bytes pushed registers + 4 bytes return PC).

```m68k
$0018e0  4a2d fccc           tst.b    -$334(a5)             ; mute / disable flag
$0018e4  66f8                bne.b    $18de                 ; return if sound disabled
$0018e6  48e7 0e0e           movem.l  d0-d2/a0-a2, -(a7)
$0018ea  302f 001c           move.w   $1c(a7), d0           ; d0 = sound_code
$0018ee  6b30                bmi.b    $1920                 ; ignore negative codes
$0018f0  3439 0001 141a      move.w   $1141a.l, d2          ; max sound code = 0x0551 (1361)
$0018f6  b042                cmp.w    d2, d0
$0018f8  6e26                bgt.b    $1920                 ; ignore out-of-range codes
$0018fa  43f9 00c0 0000      lea.l    $c00000.l, a1         ; shared RAM base
$001900  3229 0480           move.w   $480(a1), d1          ; ring producer index (doubled)
$001904  e249                lsr.w    #$1, d1               ; byte index
$001906  206d fccc           movea.l  -$338(a5), a0         ; selector pointer table ($00fed2)
$00190a  2070 0c00           movea.l  (a0, d0.w * 4), a0    ; descriptor address
$00190e  7000                moveq    #$0, d0
$001910  1018                move.b   (a0)+, d0             ; descriptor type (0..4)
$001912  303b 0a00           move.w   $18b6(pc, d0.w * 2), d0 ; dispatch jump table
$001916  4ebb 0800           jsr      $18b6(pc, d0.w)
$00191a  d241                add.w    d1, d1                ; convert byte index to doubled
$00191c  3341 0480           move.w   d1, $480(a1)          ; commit producer
$001920  4cdf 7070           movem.l  (a7)+, d0-d2/a0-a2
$001924  4e75                rts
```

## Selector pointer table and initialization

Initialized at boot routine `$0017e0` (`$0017f6`):
`move.l #$fed2, -$338(a5)`

Table spans `$00fed2..$011416` (1362 pointers, indices 0..`$0551` = 1361).
Word at `$01141a` holds table limit: `0x0551` (1361).

```m68k
$0017f6  2b7c 0000 fed2 fccc  move.l   #$fed2, -$338(a5)     ; store selector table base
$0017fe  422d fccc           clr.b    -$334(a5)
$001802  4cdf 7fff           movem.l  (a7)+, d0-d7/a0-a6
$001806  4e75                rts
```

## Ring buffer packet emitter ($001808)

Copies length-prefixed packet from scratch buffer at `-$2b0(a5)` (`0x407d50`) into shared RAM ring buffer (`& $3ff`):

```m68k
$001808  45ed fd50           lea.l    -$2b0(a5), a2         ; packet staging buffer
$00180c  7000                moveq    #$0, d0
$00180e  101a                move.b   (a2)+, d0             ; packet length N
$001810  1280 1000           move.b   d0, (a1, d1.w)        ; write length byte
$001814  5340                subq.w   #$1, d0               ; N - 1 remaining bytes
$001816  6004                bra.b    $181c
$001818  129a 1000           move.b   (a2)+, (a1, d1.w)     ; write payload byte
$00181c  5241                addq.w   #$1, d1               ; advance ring pointer
$00181e  0241 03ff           andi.w   #$3ff, d1             ; modulo 1024
$001822  51c8 fff4           dbra     d0, $1818
$001826  4e75                rts
```

## Descriptor dispatch table ($0018b6)

Jump offsets relative to `$0018b6`:
- `0x0070` -> Type 0 handler `$001926`
- `0x00e2` -> Type 1 handler `$001998`
- `0x0126` -> Type 2 handler `$0019dc`
- `0x01b6` -> Type 3 handler `$001a6c`
- `0x01cc` -> Type 4 handler `$001a82`

### Type 0 helper ($001926): sequence start (81) + volume (86)
Record layout: `[type=0, song, volume]` (3 bytes).
Emits `[03, 81, song]` followed by `[04, 86, song, volume]`. If `song & 0x80`, also emits `[04, 86, song & 0x7f, volume]`.

### Type 1 helper ($001998): sequence start (81) + volume (86) + pan (88)
Record layout: `[type=1, song, volume, pan]` (4 bytes).
Emits `[03, 81, song]`, `[04, 86, song, volume]`, and `[04, 88, song, pan]`.

### Type 2 helper ($0019dc): release old note (8f) + program (8d) + note on (8e)
Record layout: `[type=2, track, vol, ch, bank, prog, key, dur]` (8 bytes).
Emits optional `[05, 8f, track, ch, last_note]`, then `[06, 8d, track, ch, bank, prog]`, and `[06, 8e, track, ch, key, vol]`.

### Type 3 helper ($001a6c): raw packet burst
Record layout: `[type=3, byte_count - 1, packet_bytes...]`.
Directly copies `byte_count` raw packet bytes to the ring buffer.

### Type 4 helper ($001a82): NOP
Executes `rts`.

## Sound test and main-bus bankswitch ($0144f8)

Strings at `$32b6..$33a0`: `BANK`, `SOUND CODE`, `MASTER V.`, `V.INTRO`, `V.BGM`, `V.SE`, `EXIT`.
- SOUND CODE item `$35aa`: range 1..word `$fec8` (`$52b` = 1323), stored at `-$29c(a5)`.
- Category classifier `$394c`: compares sound code against threshold table at `$fea2..$fec6`:
  <=400: category 0 (BGM); 401..414: category 3; 415..561: category 2 (SE); 562..592: category 1 (Intro); etc.
- For categories 0 and 1, the sound test runs prelude pair lookup `$2e8c0(code) -> d0, d1`, requesting d0 (effect preset) and d1 (effect parameters) before requesting `code`.
- BANK item `$3406`: value 1..31 stored in RAM `$416114` (`$e114(a5)`). Pressing Enter runs `$0144f8`:

```m68k
$0144f8  3075 8170 0000 e112  movea.w  $e112(a5), a0         ; RAM $416112 (cached previous bank)
$014500  41f0 83b0 0030 0000  lea.l    $300000(a0.w * 2), a0 ; main bus address $300000 + 2*old_bank
$014508  30bc 0000           move.w   #$0, (a0)             ; clear old bank latch
$01450c  3075 8170 0000 e114  movea.w  $e114(a5), a0         ; RAM $416114 (new bank 1..31)
$014514  41f0 83b0 0030 0000  lea.l    $300000(a0.w * 2), a0 ; main bus address $300000 + 2*new_bank
$01451c  43e8 0040           lea.l    $40(a0), a1           ; secondary strobe (+$40)
$014520  30bc 0000           move.w   #$0, (a0)             ; latch new bank
$014524  32bc 0000           move.w   #$0, (a1)
$014528  3b75 8170 0000 e114  move.w   $e114(a5), $e112(a5)  ; update previous bank cache ($416112)
         8170 0000 e112
$014536  4e75                rts
```

In MAME `taito_f3.cpp`, main bus `0x300000-0x30007f` write is `sound_bankswitch_w`:
```c
int idx = (offset << 1) & 0x1e;
if (ACCESSING_BITS_0_15) idx += 1;
if (idx >= 8) idx -= 8;
m_taito_en->set_bank(1, idx); // switches sound CPU cpubank2 ($c20000-$c3ffff)
```
`cpubank2` selects a 128 KiB page from the 3 MiB sound program ROM.
- Pages 0..7 are in `e44-20`/`e44-21` (1 MiB base program).
- Pages 8..23 are in `e44-15` (2 MiB banked sequences, 16-bit word-swapped).

## Sound driver sequence opcode $58..$af disassembly ($c14772..$c14884)

The 68000 sound driver interprets sequence track event streams at `$c1475e`:
Word fetch `move.w (a0)+, d0` yields token with delay in high byte and opcode in low byte.
Opcode comparison ladder at `$c14776`:

```m68k
$c14772  c07c 00ff           and.w    #$ff, d0
$c14776  b03c 0058           cmp.b    #$58, d0              ; op < 0x58: standard note onset
$c1477a  6408                bcc.b    $c14784
$c1477c  6100 0134           bsr.w    $c148b2               ; key = op (operand 1: dur + vel; operand 2: extended dur)
$c14780  6000 0102           bra.w    $c14884
$c14784  b03c 00b0           cmp.b    #$b0, d0              ; 0x58 <= op < 0xb0: key continuation onset
$c14788  640a                bcc.b    $c14794
$c1478a  7402                moveq    #$2, d2               ; handler table index 2
$c1478c  903c 0058           sub.b    #$58, d0              ; key = op - 0x58 (keys 0..87)
$c14790  6000 00e2           bra.w    $c14874

$c14874  1940 0014           move.b   d0, $14(a4)           ; store key (op - 0x58)
$c14878  3218                move.w   (a0)+, d1             ; single operand word
$c1487a  34c1                move.w   d1, (a2)+             ; store into $c(a4) (duration = operand & 0x3ff)
$c1487c  c27c 007f           and.w    #$7f, d1              ; velocity = operand & 0x7f
$c14880  1941 0015           move.b   d1, $15(a4)           ; store velocity
$c14884  302c 000a           move.w   $a(a4), d0
...
$c148a4  3431 2000           move.w   (a1, d2.w), d2        ; jump via table at 0xc14082 + d2:
$c148a8  d4bc 00c1 0000      add.l    #$c10000, d2
$c148ae  2242                movea.l  d2, a1
$c148b0  4ed1                jmp      (a1)                  ; d2=2 jumps to $c1422c (trigger note onset)
```

At `$c1422c`:
```m68k
$c1425c  102c 0014           move.b   $14(a4), d0           ; load key (op - 0x58)
$c14260  d03c 0015           add.b    #$15, d0              ; transpose to kernel key
$c14264  1e2c 0015           move.b   $15(a4), d7           ; load velocity
$c14272  6100 0f7a           bsr.w    $c151ee               ; allocate and trigger ES5505 voice
```

In `sequencer.cpp`, opcodes `0x58..0xaf` were previously unhandled and threw:
`invalid("HLE key continuation absent from supplied data")`.
They are standard note onsets with:
`key = opcode - 0x58`
`duration = operand & 0x3ff`
`velocity = operand & 0x7f`.
Fixing this in `sequencer.cpp` resolves song `$16` (codes 19, 69, 119, 169) completely.

## Bank write trace results

Traced writes to main bus `$300000-$30007f`:
- Boot (cold boot vector `$000116`, line `$000208`):
  `PC=$000208 move.w #$0, $300002.l` -> bank 1 (page 1).
  Attract mode and title screen run entirely in Bank 1.
- Sound test menu:
  `PC=$014520 move.w #$0, (a0)` -> writes `$300000 + bank * 2` for user-selected bank (1..31).
- Gameplay stage transitions (`$02dc2e` via table `$02f22e`):
  Stage 0 sets Bank 16 (`$300020`, page 8).
  Stage 1 sets Bank 17 (`$300022`, page 9).
  Stage 2 sets Bank 18 (`$300024`, page 10).
  Stage 3 sets Bank 28 (`$300038`, page 20).
  Stage 4 sets Bank 20 (`$300028`, page 12).
  Intermission / default returns to Bank 1.

## Selector table dump (codes 1..187)

All codes 1..187 in `$00fed2` are Type 0 descriptors (`[0x00, song, volume]`) with volume `0x7f`.
Song IDs cycle: `song = 4 + (code - 1) % 50` (songs `0x04..0x35`):

```
Code   1 (0x01): ptr=0x011424 desc=00047f -> song=0x04 vol=0x7f
Code   2 (0x02): ptr=0x011427 desc=00057f -> song=0x05 vol=0x7f
Code   3 (0x03): ptr=0x01142a desc=00067f -> song=0x06 vol=0x7f
Code   4 (0x04): ptr=0x01142d desc=00077f -> song=0x07 vol=0x7f
Code   5 (0x05): ptr=0x011430 desc=00087f -> song=0x08 vol=0x7f
Code  10 (0x0a): ptr=0x01143f desc=000d7f -> song=0x0d vol=0x7f
Code  19 (0x13): ptr=0x01145a desc=00167f -> song=0x16 vol=0x7f
Code  25 (0x19): ptr=0x01146c desc=001c7f -> song=0x1c vol=0x7f
Code  28 (0x1c): ptr=0x011475 desc=001f7f -> song=0x1f vol=0x7f
Code  31 (0x1f): ptr=0x01147e desc=00227f -> song=0x22 vol=0x7f
Code  34 (0x22): ptr=0x011487 desc=00257f -> song=0x25 vol=0x7f
Code  49 (0x31): ptr=0x0114b4 desc=00347f -> song=0x34 vol=0x7f
Code  50 (0x32): ptr=0x0114b7 desc=00357f -> song=0x35 vol=0x7f
Code  51 (0x33): ptr=0x0114ba desc=00047f -> song=0x04 vol=0x7f (cycle repeats)
Code  75 (0x4b): ptr=0x011502 desc=001c7f -> song=0x1c vol=0x7f
Code 100 (0x64): ptr=0x01154d desc=00357f -> song=0x35 vol=0x7f
Code 101 (0x65): ptr=0x011550 desc=00047f -> song=0x04 vol=0x7f (cycle repeats)
Code 125 (0x7d): ptr=0x011598 desc=001c7f -> song=0x1c vol=0x7f
Code 150 (0x96): ptr=0x0115e3 desc=00357f -> song=0x35 vol=0x7f
Code 151 (0x97): ptr=0x0115e6 desc=00047f -> song=0x04 vol=0x7f (cycle repeats)
Code 175 (0xaf): ptr=0x01162e desc=001c7f -> song=0x1c vol=0x7f
Code 187 (0xbb): ptr=0x011652 desc=00287f -> song=0x28 vol=0x7f
```

## Captured packet hex per representative code

Captured mailbox packets emitted by `$0018e0` and sound test prelude:
- Code 1:
  - Prelude 0x4f9: `03 20 05` (effect preset 5)
  - Prelude 0x4fa: `04 21 00 28`, `04 21 01 00`, `04 21 02 14`, `04 21 03 1a`, `04 21 04 25`, `04 21 05 00`, `04 21 06 68`, `04 21 07 40`, `04 21 08 20`, `04 21 09 27` (effect parameters)
  - Request 0x001: `03 81 04` (start sequence 4), `04 86 04 7f` (set volume 0x7f)
- Code 2:
  - Request 0x002: `03 81 05` (start sequence 5), `04 86 05 7f` (set volume 0x7f)
- Code 19:
  - Request 0x013: `03 81 16` (start sequence 0x16), `04 86 16 7f` (set volume 0x7f)
- Code 25:
  - Request 0x019: `03 81 1c` (start sequence 0x1c), `04 86 1c 7f` (set volume 0x7f)
- Code 500 (Type 2 SFX):
  - Request 0x1f4: `06 8d 01 02 80 00` (select prog 0x8000), `06 8e 01 02 3c 70` (note on key 0x3c, vel 0x70)

## Silent vs audible results per bank (codes 1..187)

| Bank | Mapped Page | Role in Kirameki | Audible | Silent | Errors | Notes |
|---|---|---|---|---|---|---|
| Bank 1 | Page 1 (`e44-20/21`) | Boot / Attract / Menus | 168 | 19 | 0 | Songs `$1c, $1f, $22, $25, $34` have empty headers (len `$54`, zero tracks) |
| Bank 4 | Page 4 (`e44-20/21`) | Test / In-game | 187 | 0 | 0 | All 187 codes audible with opcode 0x58..0xaf HLE fix |
| Bank 16 | Page 8 (`e44-15.53`) | Stage 0 (first opponent) | 187 | 0 | 0 | 100% audible, zero silent, zero errors |
| Bank 17 | Page 9 (`e44-15.53`) | Stage 1 (second opponent) | 187 | 0 | 0 | 100% audible, zero silent, zero errors |
| Bank 18 | Page 10 (`e44-15.53`) | Stage 2 (third opponent) | 187 | 0 | 0 | 100% audible, zero silent, zero errors |
| Bank 20 | Page 12 (`e44-15.53`) | Stage 4 (fifth opponent) | 187 | 0 | 0 | 100% audible, zero silent, zero errors |
| Bank 28 | Page 20 (`e44-15.53`) | Stage 3 (fourth opponent) | 187 | 0 | 0 | 100% audible, zero silent, zero errors |

The 19 silent codes in Bank 1:
`25, 28, 31, 34, 49, 75, 78, 81, 84, 99, 125, 128, 131, 134, 149, 175, 178, 181, 184`
correspond to songs `$1c, $1f, $22, $25, $34` repeating across 50-code cycles.
In Banks 4, 16, 17, 18, 20, 28, every one of these 19 codes has populated sequence data and renders audibly.

## Reproduction

Commands to reproduce the findings:
1. Bank-write trace during attract and gameplay:
   ```sh
   mkdir -p build/f3a/kirameki-inputs
   cat << 'EOF' > build/f3a/kirameki-inputs/gameplay.txt
   700+20 coin
   750+20 start
   800-1200 mash seed=1
   EOF
   ./build-kirameki/kirameki --headless --renderer compare-cpu --frames 1200 --inputs build/f3a/kirameki-inputs/gameplay.txt
   ```
2. Sound extraction across all banks:
   ```sh
   for bank in 1 4 16 17 18 20 28; do
       ./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank $bank --sounds 1-187 --out-dir build/sounds/kirameki
   done
   ```
3. Sound bankswitch unit test:
   ```sh
   ctest --test-dir build-kirameki -R SoundBankswitchIndexMapping
   ```
