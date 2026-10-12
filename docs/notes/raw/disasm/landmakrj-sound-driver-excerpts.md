---
source_url: "Capstone script in the '## Capture script' section below (uv run python, ROMs from F3_ROM_DIR)"
ingested: 2026-10-12
sha256: 72099c588f2b373efd0f4ffef8380fe926e6a0f4cf8a98d51567b250a0abe475
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "capstone 5.0.7 (git f47dde9)"
---
# Land Maker sound driver disassembly excerpts and reset vector
Static disassembly excerpts captured from Land Maker (Japan) main ROM
(`aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc`)
and interleaved sound ROM `e61-14.32` / `e61-15.33` padded to 512 KiB (CRC32 `5a7e9117`).
## Reset vector

Sound ROM image offset `0x000004` (longword): `$00c1089e`.

```m68k
ROM offset 0x000004 longword: 0x00c1089e

$c1089e  707f        moveq    #$7f, d0
$c108a0  2200        move.l   d0, d1
$c108a2  4481        neg.l    d1
$c108a4  93c9        suba.l   a1, a1
$c108a6  22c0        move.l   d0, (a1)+
$c108a8  2281        move.l   d1, (a1)
$c108aa  b291        cmp.l    (a1), d1
$c108ac  6602        bne.b    $c108b0
$c108ae  b0a1        cmp.l    -(a1), d0
$c108b0  66f2        bne.b    $c108a4
```
## Main CPU sound selector dispatch

### Type 0 helper ($0030ee): looping start (81) then volume (86)

```m68k
$0030ee  45ed81d4    lea.l    -$7e2c(a5), a2
$0030f2  34fc0381    move.w   #$381, (a2)+
$0030f6  14d0        move.b   (a0), (a2)+
$0030f8  6100fee4    bsr.w    $2fde
$0030fc  426d81e4    clr.w    -$7e1c(a5)
$003100  426d81e6    clr.w    -$7e1a(a5)
$003104  1b68000181e7  move.b   $1(a0), -$7e19(a5)
$00310a  6100074c    bsr.w    $3858
$00310e  45ed81d4    lea.l    -$7e2c(a5), a2
$003112  34fc0486    move.w   #$486, (a2)+
$003116  14d0        move.b   (a0), (a2)+
$003118  14ed81e8    move.b   -$7e18(a5), (a2)+
$00311c  6100fec0    bsr.w    $2fde
$003120  4e75        rts      
$003122  0200007f    andi.b   #$7f, d0
```

### Type 2 helper ($0031cc): optional old-note release (8f), then program (8d) and note (8e)

```m68k
$0031cc  7000        moveq    #$0, d0
$0031ce  1010        move.b   (a0), d0
$0031d0  04400001    subi.w   #$1, d0
$0031d4  e740        asl.w    #$3, d0
$0031d6  d0280002    add.b    $2(a0), d0
$0031da  487503209f1e  pea.l    $9f1e(a5, d0.w * 2)
$0031e0  487501209f0f  pea.l    $9f0f(a5, d0.w)
$0031e6  4a7503209f1e  tst.w    $9f1e(a5, d0.w * 2)
$0031ec  671c        beq.b    $320a
$0031ee  103501209f0f  move.b   $9f0f(a5, d0.w), d0
$0031f4  6714        beq.b    $320a
$0031f6  45ed81d4    lea.l    -$7e2c(a5), a2
$0031fa  34fc058f    move.w   #$58f, (a2)+
$0031fe  14d0        move.b   (a0), (a2)+
$003200  14e80002    move.b   $2(a0), (a2)+
$003204  14c0        move.b   d0, (a2)+
$003206  6100fdd6    bsr.w    $2fde
$00320a  45ed81d4    lea.l    -$7e2c(a5), a2
$00320e  34fc068d    move.w   #$68d, (a2)+
$003212  14d0        move.b   (a0), (a2)+
$003214  14e80002    move.b   $2(a0), (a2)+
$003218  14e80003    move.b   $3(a0), (a2)+
$00321c  14e80004    move.b   $4(a0), (a2)+
$003220  6100fdbc    bsr.w    $2fde
$003224  3b7cffff81e4  move.w   #$ffff, -$7e1c(a5)
$00322a  426d81e6    clr.w    -$7e1a(a5)
$00322e  1b68000181e7  move.b   $1(a0), -$7e19(a5)
$003234  61000622    bsr.w    $3858
```

### Selector pointer table ($006bde) and music descriptors ($4e..$5d)

Max selector index stored at `$006d5e`: `0x005f`.
Descriptors `$4e..$5d` are 3-byte type 0 records `[type, song, volume]`:

```
Selector $4e: table @ $006d16 -> descriptor @ $006fd0: 000374 (type 0, song $03, vol $74)
Selector $4f: table @ $006d1a -> descriptor @ $006fd3: 00876c (type 0, song $87, vol $6c)
Selector $50: table @ $006d1e -> descriptor @ $006fd6: 000674 (type 0, song $06, vol $74)
Selector $51: table @ $006d22 -> descriptor @ $006fd9: 000874 (type 0, song $08, vol $74)
Selector $52: table @ $006d26 -> descriptor @ $006fdc: 000974 (type 0, song $09, vol $74)
Selector $53: table @ $006d2a -> descriptor @ $006fdf: 000a64 (type 0, song $0a, vol $64)
Selector $54: table @ $006d2e -> descriptor @ $006fe2: 000b70 (type 0, song $0b, vol $70)
Selector $55: table @ $006d32 -> descriptor @ $006fe5: 008e78 (type 0, song $8e, vol $78)
Selector $56: table @ $006d36 -> descriptor @ $006fe8: 000f76 (type 0, song $0f, vol $76)
Selector $57: table @ $006d3a -> descriptor @ $006feb: 009274 (type 0, song $92, vol $74)
Selector $58: table @ $006d3e -> descriptor @ $006fee: 001378 (type 0, song $13, vol $78)
Selector $59: table @ $006d42 -> descriptor @ $006ff1: 001479 (type 0, song $14, vol $79)
Selector $5a: table @ $006d46 -> descriptor @ $006ff4: 001576 (type 0, song $15, vol $76)
Selector $5b: table @ $006d4a -> descriptor @ $006ff7: 009876 (type 0, song $98, vol $76)
Selector $5c: table @ $006d4e -> descriptor @ $006ffa: 001976 (type 0, song $19, vol $76)
Selector $5d: table @ $006d52 -> descriptor @ $006ffd: 001a76 (type 0, song $1a, vol $76)
Max selector index at $006d5e: 0x005f (95)
```
## Sound CPU command dispatch and opcodes

### Opcode 82 handler ($c12f1a): stop and remove active sequence

Branches to `$c12c30`:

```m68k
$c12f1a  61ec        bsr.b    $c12f08
$c12f1c  6000fd12    bra.w    $c12c30
$c12f20  61f8        bsr.b    $c12f1a
$c12f22  6000fdbc    bra.w    $c12ce0
$c12f26  61f2        bsr.b    $c12f1a
$c12f28  6000fdbc    bra.w    $c12ce6
$c12f2c  61da        bsr.b    $c12f08
$c12f2e  6100feb6    bsr.w    $c12de6
```

### Sequence stop/remove ($c12c30) and bit 7 handling

```m68k
$c12c30  610001b4    bsr.w    $c12de6
$c12c34  3f00        move.w   d0, -(a7)
$c12c36  08800007    bclr.b   #$7, d0
$c12c3a  671a        beq.b    $c12c56
$c12c3c  61000064    bsr.w    $c12ca2
$c12c40  6714        beq.b    $c12c56
$c12c42  61a6        bsr.b    $c12bea
$c12c44  6658        bne.b    $c12c9e
$c12c46  4a280026    tst.b    $26(a0)
$c12c4a  6752        beq.b    $c12c9e
```

### Sequence bit 7 extra selection and state ($c12cb8, $c12cea)

```m68k
$c12cb8  08800007    bclr.b   #$7, d0
$c12cbc  6720        beq.b    $c12cde
$c12cbe  48a73800    movem.w  d2-d4, -(a7)
$c12cc2  61de        bsr.b    $c12ca2
$c12cc4  670c        beq.b    $c12cd2
$c12cc6  6100ff22    bsr.w    $c12bea
$c12cca  6606        bne.b    $c12cd2
$c12ccc  b47c0064    cmp.w    #$64, d2

$c12cea  610000fa    bsr.w    $c12de6
$c12cee  42786e18    clr.w    $6e18.w
$c12cf2  08800007    bclr.b   #$7, d0
$c12cf6  6750        beq.b    $c12d48
$c12cf8  61a8        bsr.b    $c12ca2
$c12cfa  674c        beq.b    $c12d48
$c12cfc  6100fd48    bsr.w    $c12a46
$c12d00  4a280026    tst.b    $26(a0)
```

### Linked sequence alias chain walk ($c12de6, $c12dfe)

Routine `$c12de6` invokes handler via `jsr (a6)` and calls `$c12dfe` to follow linked sequence aliases.
`$c12dfe` reads sequence header offset via `$c158cc` and inspects header byte `+$a6(a3)`:

```m68k
$c12de6  2c5f        movea.l  (a7)+, a6
$c12de8  3f00        move.w   d0, -(a7)
$c12dea  48e7f802    movem.l  d0-d4/a6, -(a7)
$c12dee  4e96        jsr      (a6)
$c12df0  4cdf401f    movem.l  (a7)+, d0-d4/a6
$c12df4  61000008    bsr.w    $c12dfe
$c12df8  66f0        bne.b    $c12dea
$c12dfa  301f        move.w   (a7)+, d0

$c12dfe  08800007    bclr.b   #$7, d0
$c12e02  b07c0064    cmp.w    #$64, d0
$c12e06  6412        bcc.b    $c12e1a
$c12e08  3200        move.w   d0, d1
$c12e0a  61002ac0    bsr.w    $c158cc
$c12e0e  47f01000    lea.l    (a0, d1.w), a3
$c12e12  7000        moveq    #$0, d0
$c12e14  102b00a6    move.b   $a6(a3), d0
```

### Opcode 8c handler ($c130d2): doubles index, dispatches controller ($c14330)

```m68k
$c130d2  619e        bsr.b    $c13072
$c130d4  e34b        lsl.w    #$1, d3
$c130d6  60001258    bra.w    $c14330
$c130da  6196        bsr.b    $c13072
$c130dc  1003        move.b   d3, d0
$c130de  e148        lsl.w    #$8, d0

$c14330  6100fd62    bsr.w    $c14094
$c14334  b67c0012    cmp.w    #$12, d3
$c14338  660e        bne.b    $c14348
$c1433a  983c0040    sub.b    #$40, d4
$c1433e  1384300a    move.b   d4, $a(a1, d3.w)
$c14342  d83c0040    add.b    #$40, d4
$c14346  6004        bra.b    $c1434c
$c14348  1384300a    move.b   d4, $a(a1, d3.w)
```

### Opcode 8d handler ($c130da): select track program ($c11dce)

```m68k
$c130da  6196        bsr.b    $c13072
$c130dc  1003        move.b   d3, d0
$c130de  e148        lsl.w    #$8, d0
$c130e0  1004        move.b   d4, d0
$c130e2  122e000a    move.b   $a(a6), d1
$c130e6  6000ece6    bra.w    $c11dce
$c130ea  6186        bsr.b    $c13072
$c130ec  387cd44a    movea.w  #$d44a, a4

$c11dce  50f8d8f9    st.b     $d8f9.w
$c11dd2  6004        bra.b    $c11dd8
$c11dd4  4238d8f9    clr.b    $d8f9.w
$c11dd8  48a7c000    movem.w  d0-d1, -(a7)
$c11ddc  61000594    bsr.w    $c12372
$c11de0  6100fe38    bsr.w    $c11c1a
$c11de4  6100fc24    bsr.w    $c11a0a
$c11de8  2288        move.l   a0, (a1)
```

### Opcode 8e handler ($c130ea): writes key and branches to note factory ($c140d6)

Writes key at `$14(a4)` ($c130f0), velocity at `$15(a4)` ($c130f4), duration `$7fff` at `$e(a4)` ($c130fc), and branches to note factory `$c140d6` at `$c1310a`:

```m68k
$c130ea  6186        bsr.b    $c13072
$c130ec  387cd44a    movea.w  #$d44a, a4
$c130f0  19430014    move.b   d3, $14(a4)
$c130f4  19440015    move.b   d4, $15(a4)
$c130f8  426c000c    clr.w    $c(a4)
$c130fc  397c7fff000e  move.w   #$7fff, $e(a4)
$c13102  61000f90    bsr.w    $c14094
$c13106  122e0003    move.b   $3(a6), d1
$c1310a  60000fca    bra.w    $c140d6
$c1310e  265f        movea.l  (a7)+, a3
$c13110  6100ff60    bsr.w    $c13072
$c13114  d63c0015    add.b    #$15, d3

$c140d6  4e43        trap     #$3
$c140d8  303c2700    move.w   #$2700, d0
$c140dc  a000        dc.w     $a000
$c140de  364d        movea.w  a5, a3
$c140e0  36e90024    move.w   $24(a1), (a3)+
$c140e4  334d0024    move.w   a5, $24(a1)
$c140e8  302c000c    move.w   $c(a4), d0
$c140ec  c07c03ff    and.w    #$3ff, d0
```
## Driver table access and note ownership

### Sequence slot address calculation ($c12a46): base $5e5c, stride $28

```m68k
$c12a46  307c5e5c    movea.w  #$5e5c, a0
$c12a4a  c0fc0028    mulu.w   #$28, d0
$c12a4e  41f00000    lea.l    (a0, d0.w), a0
$c12a52  7000        moveq    #$0, d0
$c12a54  4e75        rts      
$c12a56  3038d40e    move.w   $d40e.w, d0
```

### Active-sequence list ($6dfc) insertion ($c12dca)

```m68k
$c12dca  327c6dfc    movea.w  #$6dfc, a1
$c12dce  0c590000    cmpi.w   #$0, (a1)+
$c12dd2  6702        beq.b    $c12dd6
$c12dd4  60f8        bra.b    $c12dce
$c12dd6  b2fc6e14    cmpa.w   #$6e14, a1
$c12dda  6402        bcc.b    $c12dde
$c12ddc  3308        move.w   a0, -(a1)
$c12dde  303c0000    move.w   #$0, d0
```

### Sequence stream and header bank offset lookup ($c158ae, $c158cc)

`$c158ae` accesses sequence-stream bank long offset at `base + 8 + 4*ID` from `$d404`.
`$c158cc` accesses sequence-header bank word offset at `base + 8 + 2*ID` from `$d408`.

```m68k
$c158ae  e549        lsl.w    #$2, d1
$c158b0  2078d404    movea.l  $d404.w, a0
$c158b4  22301008    move.l   $8(a0, d1.w), d1
$c158b8  4e75        rts      
$c158ba  48e74000    movem.l  d1, -(a7)
$c158be  61ee        bsr.b    $c158ae

$c158cc  e349        lsl.w    #$1, d1
$c158ce  2078d408    movea.l  $d408.w, a0
$c158d2  32301008    move.w   $8(a0, d1.w), d1
$c158d6  4e75        rts      
$c158d8  7200        moveq    #$0, d1
$c158da  3238d40e    move.w   $d40e.w, d1
```

### Channel lookup ($c11c1a) and sequence slot pointer store ($c159a0)

`$c11c1a` looks up channel pointer in table `$5daa.w`.
`$c159a6` calls `$c12a46` and stores sequence slot pointer `a0` into channel `+$0e(a1)` at `$c159aa`:

```m68k
$c11c1a  43f85daa    lea.l    $5daa.w, a1
$c11c1e  e309        lsl.b    #$1, d1
$c11c20  32711000    movea.w  (a1, d1.w), a1
$c11c24  e209        lsr.b    #$1, d1
$c11c26  4e75        rts      
$c11c28  3238cf54    move.w   $cf54.w, d1

$c159a0  8080        or.l     d0, d0
$c159a2  3038d40e    move.w   $d40e.w, d0
$c159a6  6100d09e    bsr.w    $c12a46
$c159aa  3348000e    move.w   a0, $e(a1)
$c159ae  4cdf0101    movem.l  (a7)+, d0/a0
$c159b2  42290027    clr.b    $27(a1)
$c159b6  4a42        tst.w    d2
$c159b8  6b06        bmi.b    $c159c0
```

### Voice record initialization ($c17a00..$c17a46): base $d94a, stride $aa

```m68k
$c17a00  49f8d94a    lea.l    $d94a.w, a4
$c17a04  4bf8c60a    lea.l    $c60a.w, a5
$c17a08  45f8c80a    lea.l    $c80a.w, a2
$c17a0c  3947000c    move.w   d7, $c(a4)
$c17a10  41f8d844    lea.l    $d844.w, a0
$c17a14  3250        movea.w  (a0), a1
$c17a16  308c        move.w   a4, (a0)
$c17a18  39480002    move.w   a0, $2(a4)
$c17a1c  3889        move.w   a1, (a4)
$c17a1e  334c0002    move.w   a4, $2(a1)
```

### Voice initializer and note ownership ($c17b94)

Copies note node `$d81c.w` to voice `+$0e(a4)` at `$c17bc6`.
Copies channel pointer `a2` to voice `+$10(a4)` at `$c17be0`.
Loads voice scale factor from ROM table `$c08bec` into voice `+$8e(a4)` at `$c17be8..$c17bf2`:

```m68k
$c17b94  2f0d        move.l   a5, -(a7)
$c17b96  6100fdcc    bsr.w    $c17964
$c17b9a  3038d822    move.w   $d822.w, d0
$c17b9e  19400004    move.b   d0, $4(a4)
$c17ba2  903c003c    sub.b    #$3c, d0
$c17ba6  194000a8    move.b   d0, $a8(a4)
$c17baa  1038d81a    move.b   $d81a.w, d0
$c17bae  19400008    move.b   d0, $8(a4)
$c17bb2  19400009    move.b   d0, $9(a4)
$c17bb6  3978d82c000a  move.w   $d82c.w, $a(a4)
$c17bbc  3978d82a008c  move.w   $d82a.w, $8c(a4)
$c17bc2  426c00a6    clr.w    $a6(a4)
$c17bc6  3978d81c000e  move.w   $d81c.w, $e(a4)
$c17bcc  1038d81e    move.b   $d81e.w, d0
$c17bd0  19400005    move.b   d0, $5(a4)
$c17bd4  19450006    move.b   d5, $6(a4)
$c17bd8  19460007    move.b   d6, $7(a4)
$c17bdc  294b0016    move.l   a3, $16(a4)
$c17be0  394a0010    move.w   a2, $10(a4)
$c17be4  3038d83e    move.w   $d83e.w, d0
$c17be8  207c00c08bec  movea.l  #$c08bec, a0
$c17bee  10300000    move.b   (a0, d0.w), d0
$c17bf2  3940008e    move.w   d0, $8e(a4)
$c17bf6  7e00        moveq    #$0, d7
$c17bf8  4238d878    clr.b    $d878.w
```

### Sample split table selection ($c17ce6): base $c1ae76, word offsets from $c1c1e0

```m68k
$c17ce6  207c00c1c1e0  movea.l  #$c1c1e0, a0
$c17cec  3c700000    movea.w  (a0, d0.w), a6
$c17cf0  ddfc00c1ae76  adda.l   #$c1ae76, a6
$c17cf6  1038d834    move.b   $d834.w, d0
$c17cfa  b02e0005    cmp.b    $5(a6), d0
$c17cfe  6306        bls.b    $c17d06
$c17d00  dcfc000e    adda.w   #$e, a6
$c17d04  60f4        bra.b    $c17cfa
$c17d06  294e001a    move.l   a6, $1a(a4)
$c17d0a  610005e8    bsr.w    $c182f4
```
## Voice allocation and lifecycle

### Voice allocation search ($c1748e)

Searches released list at `$d850` and free list at `$d844`:

```m68k
$c1748e  48a70600    movem.w  d5-d6, -(a7)
$c17492  4278d836    clr.w    $d836.w
$c17496  3038d822    move.w   $d822.w, d0
$c1749a  7400        moveq    #$0, d2
$c1749c  7e00        moveq    #$0, d7
$c1749e  307cd850    movea.w  #$d850, a0
$c174a2  3848        movea.w  a0, a4
$c174a4  3854        movea.w  (a4), a4
$c174a6  b8c8        cmpa.w   a0, a4
$c174a8  67000036    beq.w    $c174e0
```

### Retrigger and legato voice reuse: owner change at $c1762c, visible at $c17632

Stores note owner `$d81c.w` into voice `+$0e(a4)` at `$c1762c`, immediately followed by write to `+$0a(a4)` at `$c17632`:

```m68k
$c17628  61000d02    bsr.w    $c1832c
$c1762c  3978d81c000e  move.w   $d81c.w, $e(a4)
$c17632  3978d82c000a  move.w   $d82c.w, $a(a4)
$c17638  426c00a6    clr.w    $a6(a4)
$c1763c  61000cb6    bsr.w    $c182f4
$c17640  306c0002    movea.w  $2(a4), a0
$c17644  3254        movea.w  (a4), a1
$c17646  3089        move.w   a1, (a0)
$c17648  33480002    move.w   a0, $2(a1)
$c1764c  3078d84a    movea.w  $d84a.w, a0
```

### Voice allocation exhaustion scan ($c17670, $c17678)

Scans released list `$d850` and active list `$d848` by priority class from patch `+$60` bits 6-7 saved in voice `+$97(a4)`:

```m68k
$c17670  122b0060    move.b   $60(a3), d1
$c17674  c27c00c0    and.w    #$c0, d1
$c17678  307cd850    movea.w  #$d850, a0
$c1767c  3850        movea.w  (a0), a4
$c1767e  b8c8        cmpa.w   a0, a4
$c17680  670c        beq.b    $c1768e
$c17682  4a2c0097    tst.b    $97(a4)
$c17686  67000070    beq.w    $c176f8
$c1768a  3854        movea.w  (a4), a4
$c1768c  60f0        bra.b    $c1767e
```

### Voice allocation rejection with carry set ($c176f8)

```m68k
$c176f8  b22c0097    cmp.b    $97(a4), d1
$c176fc  6608        bne.b    $c17706
$c176fe  082c0004000c  btst.b   #$4, $c(a4)
$c17704  6604        bne.b    $c1770a
$c17706  b040        cmp.w    d0, d0
$c17708  4e75        rts      
$c1770a  003c0001    ori.b    #$1, ccr
$c1770e  4e75        rts      
```

### Voice OTIS initialization ($c17e62)

Selects OTIS page, zeroes volumes, stops voice (`ori.w #$3, (a0)`), initializes filters and accumulator:

```m68k
$c17e62  316c000c001e  move.w   $c(a4), $1e(a0)
$c17e68  7000        moveq    #$0, d0
$c17e6a  21400010    move.l   d0, $10(a0)
$c17e6e  00500003    ori.w    #$3, (a0)
$c17e72  70ff        moveq    #$ff, d0
$c17e74  2140000c    move.l   d0, $c(a0)
$c17e78  00500c00    ori.w    #$c00, (a0)
$c17e7c  327c6e1c    movea.w  #$6e1c, a1
$c17e80  7200        moveq    #$0, d1
$c17e82  122c00a2    move.b   $a2(a4), d1
```

### Walk pending starts list ($c177f6) and clear stop bits ($c17814)

Walks pending list at `$d84c`. At `$c17814` clears CR stop bits 0-1 (`andi.w #$fffc, (a0)`) unless voice flags bit 4 suppresses (`btst.b #$4, $c(a4)`):

```m68k
$c177f6  3878d84c    movea.w  $d84c.w, a4
$c177fa  207c00200000  movea.l  #$200000, a0
$c17800  b8fcd84c    cmpa.w   #$d84c, a4
$c17804  6716        beq.b    $c1781c
$c17806  316c000c001e  move.w   $c(a4), $1e(a0)
$c1780c  082c0004000c  btst.b   #$4, $c(a4)
$c17812  6604        bne.b    $c17818
$c17814  0250fffc    andi.w   #$fffc, (a0)
$c17818  3854        movea.w  (a4), a4
$c1781a  60e4        bra.b    $c17800
$c1781c  3878d84c    movea.w  $d84c.w, a4
$c17820  b8fcd84c    cmpa.w   #$d84c, a4
```

### Rotating update links and splicing pending into active ($c1781c)

Updates rotating links at `+$24/+$26(a4)` and splices pending list into active list:

```m68k
$c1781c  3878d84c    movea.w  $d84c.w, a4
$c17820  b8fcd84c    cmpa.w   #$d84c, a4
$c17824  674c        beq.b    $c17872
$c17826  326c0024    movea.w  $24(a4), a1
$c1782a  306c0026    movea.w  $26(a4), a0
$c1782e  31490024    move.w   a1, $24(a0)
$c17832  33480026    move.w   a0, $26(a1)
$c17836  b8f8d85a    cmpa.w   $d85a.w, a4
$c1783a  6606        bne.b    $c17842
$c1783c  31c9d85a    move.w   a1, $d85a.w
```

### Unlink freed voice to $d844 and silence hardware ($c1789a, $c17a88)

Unlinks voice, returns it to free list `$d844`, and silences hardware via `$c17a88`:

```m68k
$c1789a  3f09        move.w   a1, -(a7)
$c1789c  306c0002    movea.w  $2(a4), a0
$c178a0  3254        movea.w  (a4), a1
$c178a2  3089        move.w   a1, (a0)
$c178a4  33480002    move.w   a0, $2(a1)
$c178a8  41f8d844    lea.l    $d844.w, a0
$c178ac  3250        movea.w  (a0), a1
$c178ae  308c        move.w   a4, (a0)
$c178b0  39480002    move.w   a0, $2(a4)
$c178b4  3889        move.w   a1, (a4)

$c17a88  7000        moveq    #$0, d0
$c17a8a  1940000c    move.b   d0, $c(a4)
$c17a8e  306c0022    movea.w  $22(a4), a0
$c17a92  21400100    move.l   d0, $100(a0)
$c17a96  20c0        move.l   d0, (a0)+
$c17a98  21400100    move.l   d0, $100(a0)
$c17a9c  20c0        move.l   d0, (a0)+
$c17a9e  207c00200000  movea.l  #$200000, a0
```
## Hardware initialization and DSP effects worker

### DUART initialization ($c10e22): counter $07d0, ACR $60, IMR $2b

Writes counter `$07d0` at `$c10e66..$c10e6a` via MOVEP, ACR `$60` at `$c10e6e`, IMR `$2b` at `$c10e76`:

```m68k
$c10e22  615a        bsr.b    $c10e7e
$c10e24  117c0000000a  move.b   #$0, $a(a0)
$c10e2a  615a        bsr.b    $c10e86
$c10e2c  117c00100004  move.b   #$10, $4(a0)
$c10e32  117c00130000  move.b   #$13, $0(a0)
$c10e38  117c00070000  move.b   #$7, $0(a0)
$c10e3e  117c00ee0002  move.b   #$ee, $2(a0)
$c10e44  117c00010004  move.b   #$1, $4(a0)
$c10e4a  117c00300004  move.b   #$30, $4(a0)
$c10e50  117c00080004  move.b   #$8, $4(a0)

$c10e5c  303c09c4    move.w   #$9c4, d0
$c10e60  4a38d528    tst.b    $d528.w
$c10e64  6704        beq.b    $c10e6a
$c10e66  303c07d0    move.w   #$7d0, d0
$c10e6a  0188000c    movep.w  d0, $c(a0)
$c10e6e  117c00600008  move.b   #$60, $8(a0)
$c10e74  6108        bsr.b    $c10e7e
$c10e76  117c002b000a  move.b   #$2b, $a(a0)
$c10e7c  4e75        rts      
$c10e7e  207c00280001  movea.l  #$280001, a0
```

### DSP worker queue post ($c0f430): queue at $fb28

Posts task message to queue `$fb28.w` via TRAP #9:

```m68k
$c0f430  3f00        move.w   d0, -(a7)
$c0f432  4e43        trap     #$3
$c0f434  3b5f0002    move.w   (a7)+, $2(a5)
$c0f438  327cfb28    movea.w  #$fb28, a1
$c0f43c  4e49        trap     #$9
$c0f43e  4e75        rts      
$c0f440  6132        bsr.b    $c0f474
$c0f442  b038d790    cmp.b    $d790.w, d0
```

### Effects configuration selection: $c0f482 and $c0f4bc

Routine `$c0f482` multiplies configuration index `d0` by 16 (`mulu.w #$10, d0`) and indexes 13-entry configuration table at `$c1c2d2` into `a4`.
Worker routine `$c0f4bc` bounds-checks, invokes `$c0f482` via `$c0f440`, records configuration pointer `a4` at `$d78c.w` and ID at `$d790.w`, and invokes DSP program loader:

```m68k
$c0f482  3200        move.w   d0, d1
$c0f484  c0fc0010    mulu.w   #$10, d0
$c0f488  287c00c1c2d2  movea.l  #$c1c2d2, a4
$c0f48e  d8c0        adda.w   d0, a4
$c0f490  4e75        rts      
$c0f492  61e0        bsr.b    $c0f474

$c0f4bc  61ea        bsr.b    $c0f4a8
$c0f4be  4e48        trap     #$8
$c0f4c0  7000        moveq    #$0, d0
$c0f4c2  4e47        trap     #$7
$c0f4c4  4a78fb36    tst.w    $fb36.w
$c0f4c8  6702        beq.b    $c0f4cc
$c0f4ca  61dc        bsr.b    $c0f4a8
$c0f4cc  6100ff72    bsr.w    $c0f440
$c0f4d0  6572        bcs.b    $c0f544
$c0f4d2  11c1d790    move.b   d1, $d790.w
$c0f4d6  21ccd78c    move.l   a4, $d78c.w
$c0f4da  7000        moveq    #$0, d0
```
## Sequence event decoding

### Event loop ($c14730, $c1475a): scan big-endian words for bit 15 set

Scans big-endian words at `$c1475e..$c14760` with `move.w (a0)+, d0; bpl.b $c1475e`:

```m68k
$c14730  387cd44a    movea.w  #$d44a, a4
$c14734  29480004    move.l   a0, $4(a4)
$c14738  29490000    move.l   a1, $0(a4)
$c1473c  08b80000d463  bclr.b   #$0, $d463.w
$c14742  206c0004    movea.l  $4(a4), a0
$c14746  6112        bsr.b    $c1475a
$c14748  387cd44a    movea.w  #$d44a, a4
$c1474c  08380000d463  btst.b   #$0, $d463.w
$c14752  67ee        beq.b    $c14742
$c14754  206c0004    movea.l  $4(a4), a0

$c1475a  4238d4be    clr.b    $d4be.w
$c1475e  3018        move.w   (a0)+, d0
$c14760  6afc        bpl.b    $c1475e
$c14762  3940000a    move.w   d0, $a(a4)
$c14766  45ec000c    lea.l    $c(a4), a2
$c1476a  226c0000    movea.l  $0(a4), a1
```
## Fixed driver tables in ROM

### Signed scale multipliers ($c089dc)

Signed multipliers used in volume fades, pitch calculations and envelopes:

```
ROM $c089dc hex: 7fff7fff40002aaa20001999155512f6112610000eee0dee0cff0c200b500a8e
Signed 16-bit words: [32767, 32767, 16384, 10922, 8192, 6553, 5461, 4854, 4390, 4096, 3822, 3566, 3327, 3104, 2896, 2702]
```

### Additional voice scaling table ($c08bec)

Voice scaling byte table loaded into voice record `+$8e(a4)` at `$c17be8..$c17bf2`:

```
ROM $c08bec hex: 00070e151c242b32393c4044474b4e5255595c6064676b6e7275797c8084878b
Bytes (first 16): [0, 7, 14, 21, 28, 36, 43, 50, 57, 60, 64, 68, 71, 75, 78, 82]
```

### Sample loop-mode to OTIS control byte lookup ($c0a734)

Control byte lookup indexed by loop mode in voice programming (`$c17f6a`, `$c17fa8`):

```
ROM $c0a734 hex: 030b0b1b0b00000001030507090b0d0f
Bytes: [3, 11, 11, 27, 11, 0, 0, 0]
Bit meanings:
  03 = 0b00000011 (bits 0-1 set: stop)
  0b = 0b00001011 (bit 3 set: loop enable, bits 0-1 set: stop)
  1b = 0b00011011 (bit 4 set: bi-directional loop, bit 3 set: loop enable, bits 0-1 set: stop)
```
## Capture script

The excerpts above were captured using Capstone 5.0.7 in M68K mode (68000 for sound, 68020 for main)
from the supplied Land Maker ROMs via the following script:

```python
import hashlib, zlib
from pathlib import Path
import capstone as cs

rom_dir = Path("roms/landmakr")
h = (rom_dir / "e61-14.32").read_bytes()
l = (rom_dir / "e61-15.33").read_bytes()
snd_rom = bytearray(0x80000)
snd_rom[0::2] = h + b"\xff" * (0x40000 - len(h))
snd_rom[1::2] = l + b"\xff" * (0x40000 - len(l))
assert zlib.crc32(snd_rom) == 0x5a7e9117

main_rom = bytearray(0x200000)
for f, o in [("e61-13.20", 0), ("e61-12.19", 1), ("e61-11.18", 2), ("e61-10.17", 3)]:
    main_rom[o::4] = (rom_dir / f).read_bytes()
assert hashlib.sha256(main_rom).hexdigest() == "aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc"

md68k = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_M68K_000)
md68k.detail = True
md020 = cs.Cs(cs.CS_ARCH_M68K, cs.CS_MODE_M68K_020)
md020.detail = True

def dis_snd(addr, count):
    off = addr - 0xc00000
    code = bytes(snd_rom[off:off + count * 8])
    return "\n".join(f"${i.address:06x}  {i.bytes.hex():<10}  {i.mnemonic:<8} {i.op_str}"
                     for i in list(md68k.disasm(code, addr))[:count])

def dis_main(addr, count):
    code = bytes(main_rom[addr:addr + count * 8])
    return "\n".join(f"${i.address:06x}  {i.bytes.hex():<10}  {i.mnemonic:<8} {i.op_str}"
                     for i in list(md020.disasm(code, addr))[:count])
```
