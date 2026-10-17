---
source_url: "Capstone M68K disassembly of roms/landmakrj main ROM plus f3rt-tool sound-extract runs (commands in '## Reproduction')"
ingested: 2026-10-16
sha256: 797744d52a45dcfd105308ef6bac1be64b24ab57d8a0ed9da045f04690eedd02
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "capstone 5.0.7 (git f47dde9)"
---
# Land Maker sound request ($00308c), SOUND TEST and selector table

Static excerpts from the Land Maker (Japan) main ROM (`aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc`),
used by `f3rt-tool sound-extract --sound CODE`. The `f3rt-tool` profile for `landmakrj` calls `$00308c`
exactly as the SOUND TEST does; it does not re-encode packets.

## Main CPU sound request entry ($00308c)

Signature: `void sound_play(uint16_t code)`. Callers push the word code: `move.w #code, -(a7); jsr $308c.l; addq.l #2, a7`
(69 call sites). The code is the selector index into the descriptor table. Codes greater than the ROM word at `$006d5e`
(`0x005f`) are ignored; RAM byte `$400150` (`-$7eb0(a5)`, cleared by `$002f84`) mutes the routine.
The selector table base lives in RAM longword `$40014c` (`-$7eb4(a5)`), set by `$002f84`:
`$006bde` (ROM table, used here) or `$400890` when the backup RAM holds the magic `TAITO   ` (longs `$54414954`, `$4f202020`) at `$400888`
(user-modified sound data saved from the test menu; absent after a fresh boot).

```m68k
$00308c  48e7e0e0         movem.l d0-d2/a0-a2, -(a7)
$003090  4a2d8150         tst.b -$7eb0(a5)
$003094  663a             bne.b $30d0
$003096  302f001c         move.w $1c(a7), d0
$00309a  6b34             bmi.b $30d0
$00309c  343900006d5e     move.w $6d5e.l, d2
$0030a2  b042             cmp.w d2, d0
$0030a4  6e2a             bgt.b $30d0
$0030a6  43f900c00000     lea.l $c00000.l, a1
$0030ac  32290480         move.w $480(a1), d1
$0030b0  e249             lsr.w #$1, d1
$0030b2  206d814c         movea.l -$7eb4(a5), a0
$0030b6  20700400         movea.l (a0, d0.w * 4), a0
$0030ba  7000             moveq #$0, d0
$0030bc  1018             move.b (a0)+, d0
$0030be  303b0216         move.w $30d6(pc, d0.w), d0
$0030c2  4e71             nop 
$0030c4  4e71             nop 
$0030c6  4ebb000e         jsr $30d6(pc, d0.w)
$0030ca  d241             add.w d1, d1
$0030cc  33410480         move.w d1, $480(a1)
$0030d0  4cdf0707         movem.l (a7)+, d0-d2/a0-a2
$0030d4  4e75             rts
```

Dispatch word table at `$0030d6` (offsets relative to `$0030d6`): type 0 `$0030e0`, type 1 `$003176`, type 2 `$0031cc`,
type 3 `$003270`, type 4 `$003286`. The producer index is read at `$c00480` and committed by the common tail; packets
are emitted by `$002fde`.

### Selector init ($002f84)

```m68k
$002f84  48e7fffe         movem.l d0-d7/a0-a6, -(a7)
$002f88  33fc000000c80100 move.w #$0, $c80100.l
$002f90  41f900c00000     lea.l $c00000.l, a0
$002f96  303c01ff         move.w #$1ff, d0
$002f9a  7200             moveq #$0, d1
$002f9c  20c1             move.l d1, (a0)+
$002f9e  51c8fffc         dbra d0, $2f9c
$002fa2  0cb95441495400400888 cmpi.l #$54414954, $400888.l
$002fac  6616             bne.b $2fc4
$002fae  0cb94f2020200040088c cmpi.l #$4f202020, $40088c.l
$002fb8  660a             bne.b $2fc4
$002fba  2b7c00400890814c move.l #$400890, -$7eb4(a5)
$002fc2  6008             bra.b $2fcc
$002fc4  2b7c00006bde814c move.l #$6bde, -$7eb4(a5)
$002fcc  422d8150         clr.b -$7eb0(a5)
$002fd0  33fc000000c80000 move.w #$0, $c80000.l
$002fd8  4cdf7fff         movem.l (a7)+, d0-d7/a0-a6
```

## SOUND TEST (menu code at $100b40..$100f12, strings at $102ed7..)

Strings: `SOUND TEST`, `SOUND NO     :%3.3X`, `SOUND VOLUME :`, `SOUND BALANCE:`, `SAVE MODIFIED DATA`, `CODE VOLUME :`,
`TRACK %1d     :`. The SOUND NO item (`$100c86`..`$100d0c`) edits a 3-digit hex number in `-$18(a6)` with the shared
number chooser `$1009de(input, current, min=1, max=word $6d5e, step=1)`, so the test shows `001`..`05F` and the number
is the selector code itself (`0x5C` = 92 = `SOUND NO : 05C`). The fire button (`$4000b9` bit 4) requests the code:

```m68k
$100cae  2d40ffe8         move.l d0, -$18(a6)
$100cb2  1039004000b9     move.b $4000b9.l, d0
$100cb8  02000010         andi.b #$10, d0
$100cbc  6722             beq.b $100ce0
$100cbe  2003             move.l d3, d0
$100cc0  5280             addq.l #$1, d0
$100cc2  670a             beq.b $100cce
$100cc4  3f03             move.w d3, -(a7)
$100cc6  4eb900003288     jsr $3288.l
$100ccc  548f             addq.l #$2, a7
$100cce  202effe8         move.l -$18(a6), d0
$100cd2  3f00             move.w d0, -(a7)
$100cd4  4eb90000308c     jsr $308c.l
$100cda  548f             addq.l #$2, a7
$100cdc  262effe8         move.l -$18(a6), d3
```

`$3288` (stop of the previously played code, skipped while `d3 == -1`) runs only from the second press on, so the first
code from a fresh boot is requested alone. There is no category step and no prelude pair: the effect preset and effect
parameters are the type 3 descriptors `$5e` and `$5f` below, which the game itself sends during boot (13 HLE commands are
in the post-boot state). A second SOUND NO item (`$101192`..`$1011de`, with CODE VOLUME / TRACK) uses the same range and the
same `$308c` call (`$1011d6`).

## Descriptor table ($006bde, entries $00..$5f; type 0 = start+volume, 2 = program+note, 3 = raw packets)

Descriptor types in `$00..$5f`: 77 x type 2, 16 x type 0, 3 x type 3 (`$00`, `$5e`, `$5f`).
Format: `selector  table-slot -> descriptor  type  bytes` (type 3 shows `[3, n-1, packet bytes...]`).

```
$00  $006bde -> $006d60  type 3  030603800103800202
$01  $006be2 -> $006d68  type 2  0201640140002702
$02  $006be6 -> $006d70  type 2  0202500840002902
$03  $006bea -> $006d78  type 2  0201480240002b02
$04  $006bee -> $006d80  type 2  0202500840002c02
$05  $006bf2 -> $006d88  type 2  0202500840002e02
$06  $006bf6 -> $006d90  type 2  0202500840003002
$07  $006bfa -> $006d98  type 2  0201600140022e02
$08  $006bfe -> $006da0  type 2  0201600140023002
$09  $006c02 -> $006da8  type 2  0201600140003302
$0a  $006c06 -> $006db0  type 2  0202500840003502
$0b  $006c0a -> $006db8  type 2  0202500840003702
$0c  $006c0e -> $006dc0  type 2  02026e0640012702
$0d  $006c12 -> $006dc8  type 2  0201600440012902
$0e  $006c16 -> $006dd0  type 2  02015e0840012b02
$0f  $006c1a -> $006dd8  type 2  0202500840012c02
$10  $006c1e -> $006de0  type 2  0202500840012e02
$11  $006c22 -> $006de8  type 2  0202500840013002
$12  $006c26 -> $006df0  type 2  0202500840013202
$13  $006c2a -> $006df8  type 2  0202540140013502
$14  $006c2e -> $006e00  type 2  02026c0240013302
$15  $006c32 -> $006e08  type 2  02016e0840013702
$16  $006c36 -> $006e10  type 2  0201680740022702
$17  $006c3a -> $006e18  type 2  0201620340022902
$18  $006c3e -> $006e20  type 2  0202500840022b02
$19  $006c42 -> $006e28  type 2  0201620140022c02
$1a  $006c46 -> $006e30  type 2  0202500840013202
$1b  $006c4a -> $006e38  type 2  0201640140022b02
$1c  $006c4e -> $006e40  type 2  0201680140032702
$1d  $006c52 -> $006e48  type 2  0202640640032901
$1e  $006c56 -> $006e50  type 2  0202640540032b02
$1f  $006c5a -> $006e58  type 2  0202680440032c02
$20  $006c5e -> $006e60  type 2  02016c0840032e02
$21  $006c62 -> $006e68  type 2  0201680840033002
$22  $006c66 -> $006e70  type 2  0201500840033202
$23  $006c6a -> $006e78  type 2  02026c0440033302
$24  $006c6e -> $006e80  type 2  0202620640033502
$25  $006c72 -> $006e88  type 2  0202500840033702
$26  $006c76 -> $006e90  type 2  0201000840082e02
$27  $006c7a -> $006e98  type 2  02016e0140082e02
$28  $006c7e -> $006ea0  type 2  02016e0140082e02
$29  $006c82 -> $006ea8  type 2  02016e0140082e02
$2a  $006c86 -> $006eb0  type 2  0201660540042702
$2b  $006c8a -> $006eb8  type 2  0201680540042902
$2c  $006c8e -> $006ec0  type 2  02016a0540042b02
$2d  $006c92 -> $006ec8  type 2  02016a0540042c02
$2e  $006c96 -> $006ed0  type 2  0201660540042e02
$2f  $006c9a -> $006ed8  type 2  0201680540043002
$30  $006c9e -> $006ee0  type 2  0201680540043202
$31  $006ca2 -> $006ee8  type 2  0201680540043302
$32  $006ca6 -> $006ef0  type 2  0201680540043502
$33  $006caa -> $006ef8  type 2  0201680540043702
$34  $006cae -> $006f00  type 2  0201680540043802
$35  $006cb2 -> $006f08  type 2  0201640640052702
$36  $006cb6 -> $006f10  type 2  0201660640052902
$37  $006cba -> $006f18  type 2  0201660640052b02
$38  $006cbe -> $006f20  type 2  0201660640052c02
$39  $006cc2 -> $006f28  type 2  0201640640052e02
$3a  $006cc6 -> $006f30  type 2  0201660640053002
$3b  $006cca -> $006f38  type 2  0201660640053202
$3c  $006cce -> $006f40  type 2  0201660640053302
$3d  $006cd2 -> $006f48  type 2  0201660640053502
$3e  $006cd6 -> $006f50  type 2  0201660640053702
$3f  $006cda -> $006f58  type 2  0201660640053802
$40  $006cde -> $006f60  type 2  02016e0640043a02
$41  $006ce2 -> $006f68  type 2  02016a0640053a02
$42  $006ce6 -> $006f70  type 2  0201660640002702
$43  $006cea -> $006f78  type 2  0201660640032702
$44  $006cee -> $006f80  type 2  0201660640042702
$45  $006cf2 -> $006f88  type 2  0201660640052702
$46  $006cf6 -> $006f90  type 2  0201660640103f02
$47  $006cfa -> $006f98  type 2  0201660640282702
$48  $006cfe -> $006fa0  type 2  0201660640272702
$49  $006d02 -> $006fa8  type 2  02016606400d2702
$4a  $006d06 -> $006fb0  type 2  0201660640552702
$4b  $006d0a -> $006fb8  type 2  0201660640142702
$4c  $006d0e -> $006fc0  type 2  02016606404b2702
$4d  $006d12 -> $006fc8  type 2  0201660640472702
$4e  $006d16 -> $006fd0  type 0  000374
$4f  $006d1a -> $006fd3  type 0  00876c
$50  $006d1e -> $006fd6  type 0  000674
$51  $006d22 -> $006fd9  type 0  000874
$52  $006d26 -> $006fdc  type 0  000974
$53  $006d2a -> $006fdf  type 0  000a64
$54  $006d2e -> $006fe2  type 0  000b70
$55  $006d32 -> $006fe5  type 0  008e78
$56  $006d36 -> $006fe8  type 0  000f76
$57  $006d3a -> $006feb  type 0  009274
$58  $006d3e -> $006fee  type 0  001378
$59  $006d42 -> $006ff1  type 0  001479
$5a  $006d46 -> $006ff4  type 0  001576
$5b  $006d4a -> $006ff7  type 0  009876
$5c  $006d4e -> $006ffa  type 0  001976
$5d  $006d52 -> $006ffd  type 0  001a76
$5e  $006d56 -> $007000  type 3  030303200b03
$5f  $006d5a -> $007005  type 3  03280421001e0421013a04210249042103200421042504210500042106280421072b0421081204210920ff
```

Type 0 helper `$0030ee`/`$0030e0` and type 2 helper `$0031cc` are listed in `landmakrj-sound-driver-excerpts.md`.
Type 0 record `[0, song, volume]` emits `03 81 song` then `04 86 song volume`; type 2 helper `$0031cc` emits an
optional `05 8f` release of the previous note on the track, then `06 8d` (program) and `06 8e` (note on); selector `$5e` (type 3) emits the effect
preset `03 20 0b` and `$5f` the ten effect parameters `04 21 idx val`.

## Captured packets (HLE default backend, post-boot snapshot)

Columns: code, descriptor type, packets the game's `$00308c` wrote to the mailbox ring (hex).

```
0x01  type 2  068d01014000 068e01012764
0x02  type 2  068d02084000 068e02082950
0x1c  type 2  068d01014003 068e01012768
0x4e  type 0  03810304860374
0x5c  type 0  03811904861976
0x5e  type 3  03200b                (effect preset; silent alone)
0x5f  type 3  0421001e 0421013a 04210249 04210320 04210425 04210500 04210628 0421072b 04210812 04210920 (effect parameters; silent alone)
```

Selector `0x5c` (type 0, song `$19`, volume `$76`) renders 20.000 s (the `--seconds` cap, a loop) with peak 2677 on the HLE
backend and 19.998 s, peak 2909 on `--audio-backend reference --sound-driver native`. Sweep `--sounds 1-0x5f`:
93 audible, 2 silent (`0x5e`, `0x5f`), 0 errors; `0x60` fails with "exceeds the game's highest sound code 0x5f".
Reference vs HLE for the other codes: `0x01` 0.313 s / 0.315 s, `0x1c` 1.336 s / 1.334 s, `0x4e` 20.0 s both; all audible.

## Reproduction

```sh
# disassembly: capstone M68K over the interleaved main ROM roms/landmakrj (e61-13.20, e61-12.19, e61-11.18, e61-10.17)
cmake --build build-landmakrj --target f3rt-tool
./build-landmakrj/f3rt-tool sound-extract --sound 0x5C --wav /tmp/lm/5c.wav
./build-landmakrj/f3rt-tool sound-extract --sound 0x5C --audio-backend reference --sound-driver native --wav /tmp/lm/5c_ref.wav
./build-landmakrj/f3rt-tool sound-extract --sounds 1-0x5f --out-dir /tmp/lm/all   # index.csv lists the packets per code
./build-landmakrj/f3rt-tool sound-extract --bank 1 --sound 0x5C --wav /tmp/lm/x.wav
# SOUND_EXTRACT ERROR: set 'landmakrj' has no sound bank switching; omit --bank
```
