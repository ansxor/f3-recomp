---
source_url: "tools/f3a g.run(frames=6000 ... out=build/f3a/board-probe-a) and g.run(frames=2950 ... out=build/f3a/board-probe-b), commands below"
ingested: 2026-10-15
sha256: 4e276aa5c9d8f2ced0b247ab2cda34a4f0ab677b76e6ad32fec27d1ea57a7e7d
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "tools/f3a (git d70a4a0 + uncommitted), runtime headless"
---
# Land Maker (landmakrj) board RAM and piece queue probe

Two headless 1P-versus-CPU runs with mashed input, read back through `tools/f3a` dumps, plus the static
disassembly of the board and piece-generator routines. Dumps are build output and were not retained;
the values below were re-read from `build/f3a/board-probe-a` and `build/f3a/board-probe-b` on 2026-10-15.

## Input script (build/f3a/landmakrj-inputs/coin-start-mash.txt, verbatim)

```
# coin, then start every 90 frames, then mash
700+20 coin
800-2400 mash keys=start period=45 seed=7
1200-end mash seed=3
```

## Runs

Python kernel: `from tools.analysis import f3a; g = f3a.game("landmakrj")`.

Run A (board-probe-a):
```
g.run(frames=6000, inputs="build/f3a/landmakrj-inputs/coin-start-mash.txt", dump_start=1000, dump_every=50, keep="mainram.bin,rendered.png", out="build/f3a/board-probe-a")
```
```
VIDEO game_frames=6000 oracle_fallback_frames=0
set=landmakrj frames=6000 pc=0x1136 ... fallback_instructions=0 ...
FINAL state_crc=0x33d3528c
```

Run B (board-probe-b):
```
g.run(frames=2950, inputs="build/f3a/landmakrj-inputs/coin-start-mash.txt", dump_start=1750, dump_every=2, keep="mainram.bin", out="build/f3a/board-probe-b")
```
```
VIDEO game_frames=2950 oracle_fallback_frames=0
set=landmakrj frames=2950 pc=0x1136 ... fallback_instructions=0 ...
FINAL state_crc=0x2b357b97
```

Gameplay was taken to be frames about 1750..3700: word 0x401f40 = 0x0100 and byte 0x401f53 = 1 in the dumps
at frames 1700 and 1750 of run A (0x401f40 = 0x1100 at frame 3700). Git HEAD at capture: d70a4a0 with
uncommitted changes in `runtime/frontend/state.hpp` and `runtime/rom.cpp` (not touched by this probe).

## Static disassembly (g.dis, doc-level evidence)

`sub_09c854` (board init), 0x09c854..0x09c8de:
```
0x09c854  lea.l -$c4a(a5), a0        ; 0x4073b6
0x09c858  lea.l -$98a(a5), a1        ; 0x407676
0x09c85c  move.w #$9f, d0
0x09c860  clr.l (a0)+ / 0x09c862 clr.l (a1)+ / 0x09c864 dbra d0, $9c860      ; 0xa0 longs per player
0x09c870  move.w #$10, d0
0x09c874  move.w #$ffff, (a0)        ; also $4(a0), $1c(a0), same for a1
0x09c894  btst.b #$0, d0 / beq.b $9c8a6
0x09c89a  move.w #$ffff, $18(a0)     ; and $18(a1), odd d0 only
0x09c8a6  lea.l $20(a0), a0 / lea.l $20(a1), a1 / dbra d0, $9c874
0x09c8b2  move.w #$7, d0
0x09c8b6  move.w #$ffff, (a0) / (a1) ; lea.l $4(a0),a0 / lea.l $4(a1),a1 / dbra d0, $9c8b6   ; row 17, 8 cells
0x09c8ca  lea.l -$c8a(a5), a0        ; 0x407376
0x09c8ce  lea.l -$9ca(a5), a1        ; 0x407636
0x09c8d2  move.w #$f, d0 / clr.l (a0)+ / clr.l (a1)+ / dbra                   ; 16 longs each
```

`sub_09c244` (and `sub_09c260`): `0x09c24c  andi.l #$ff000000, (a0, d0.w * 4)` where d0 += d1 << 3.

`sub_09c474` (neighbour check), selected lines:
```
0x09c474  move.w $36(a3), d0    0x09c478 move.w $38(a3), d1    0x09c47c move.w $26(a3), d4
0x09c486  addq.w #$2, d5        (d5 = d0 + 2)
0x09c488  tst.w $24(a3) / bne.b $9c498     ; a0 = 0x4073b6, a1 = 0x401fa4 or a0 = 0x407676, a1 = 0x40204c
0x09c4a0  lsl.w #$3, d6 / add.w d5, d6
0x09c4a4  move.l a5, d3
0x09c4a6  move.w $40(a0, d6.w), d3         ; index scale *4
0x09c4aa  cmpi.w #$b0, d3 / blt  0x09c4b0 cmpi.w #$b6, d3 / bgt
0x09c4b6  move.w $42(a0, d6.w), d3 / bsr.w $9c52e
0x09c4cc  btst.b #$0, d1 / bne.b $9c4e0    ; odd row: $24/$26 else $1c/$1e, plus $20/$22
```

`sub_0938f2` (queue shift), 0x0938f2..0x093958:
```
0x0938f2  tst.l $50(a2) / bne.b $93958
0x09390a  bsr.w $93a1c                     ; sub_093a1c
0x09390e  move.w $92(a2), d4
0x093912  move.w d0, $92(a2)
0x093916  move.w d4, $90(a2)
0x093932  move.l #$ffffffff, $50(a2)
```

`sub_093462` (round init), first lines: clr.w -$5f04(a5) (0x4020fc); (a0) = 0x401fa4 gets word 0; (a1) =
0x40204c gets word 1; then the seed calls of `sub_093a1c`/`sub_0938f2` follow.

`sub_093a1c` (generator), 0x093a1c..0x093af6:
```
0x093a1c  cmpi.w #$ffff, -$60ac(a5)  / beq.w $93996      ; stage word 0x401f54
0x093a26  cmpi.w #$fffe, -$60ac(a5)  / beq.b $939dc
0x093a3a  tst.w -$60a4(a5) / bne.w $93ae6                ; 0x401f5c: round winner -> colour 0
0x093a42  move.l -$77da(a5), d7                          ; 0x400826 saved
0x093a46  move.l $98(a2), -$77da(a5)                     ; per-player seed loaded
0x093a4c  tst.w $94(a2) / bne.b $93aae
0x093a52  move.w #$1, $94(a2)
0x093a5c  lea.l -$c4a(a5), a0 / lea.l -$ea4(a5), a1      ; P1 board, mask 0x40715c
0x093a66  lea.l -$98a(a5), a0 / lea.l -$ea2(a5), a1      ; P2 board, mask 0x40715e
0x093a6e  clr.w (a1) / move.w #$13, d2 / move.w #$7, d1  ; 20 rows x 8 cells
0x093a78  tst.w $2(a0) / beq.b $93a96
0x093a7e  move.w (a0), d0 / cmpi.w #$b0, d0 / blt.b $93a8c / cmpi.w #$b6, d0 / ble.b $93a96
0x093a8c  andi.w #$f, d0 / asr.w #$1, d0 / bset.b d0, $1(a1)
0x093aae  move.w #$0, $94(a2) ; mask re-read from 0x40715c / 0x40715e
0x093ac6  jsr $1468.l / ext.l d0 / divu.w #$7, d0 / swap d0
0x093ad4  move.w $93af8(pc, d0.w), d0                    ; index scale *2
0x093adc  move.w d0, d2 / asr.w #$1, d0 / btst.l d0, d1 / beq.b $93ac6
0x093aea  move.l -$77da(a5), $98(a2)
0x093af0  move.l d7, -$77da(a5)
0x093af4  move.w d2, d0 / rts
```

## Board dump observations (build/f3a/board-probe-a, board-probe-b; 4 bytes per cell, big-endian)

Run A frame 3250, P1 board (cells shown as `0xFFTTPPPP` longs; row 17 omitted = eight 0xffff0000):
```
row  0: ffff0000 ffff0000 0 0 0 0 0 ffff0000
row  1: ffff0000 ffff0000 0 0 0 0 ffff0000 ffff0000
row  2: ffff0000 ffff0000 0 0 8a4158 842d68 0 ffff0000
row  3: ffff0000 ffff0000 843108 8a3ffc 8a2c80 842cf4 ffff0000 ffff0000
...
row 12: ffff0000 ffff0000 8439a4 0 843a18 843a8c 843b74 ffff0000
row 13: ffff0000 ffff0000 ff000000 ff8438bc ff000000 ff000000 ffff0000 ffff0000
row 15: ffff0000 ffff0000 ff843be8 ff843d44 ff843f14 ff000000 ffff0000 ffff0000
row 16: ffff0000 ffff0000 ff000000 ff843cd0 ff843ea0 ff8a3f88 ff000000 ffff0000
row 17: ffff0000 x8
```
Rows 13..16 carry byte0 = 0xff; the same board at frame 3000 has rows 0 and 1 as above and row 16
`ffff0000 ffff0000 ff000000 ff843848 ff843930 ff000000 ff000000 ffff0000`.
Launcher rows (0x407376..0x4073b5) all zero at frame 3000.

Run B fired block (P1 cell value `0x00843e2c`): row 1 col 3 at frame 2002, row 5 col 3 at 2008, row 8 col 4 at
2016. Held block in the launcher: `008a3f14` at 0x40737e (row -2, col 2) at frame 2066.

Run B queue words (P1: +0x90 0x402034, +0x92 0x402036, mask 0x40715c, +0x50 ptr 0x401ff4):
```
frame  +0x90  +0x92  mask   +0x50
1750   0006   0006   0088   004038bc
1870   0004   000a   0024   ffffffff
1998   000a   000a   0024   00000000
2066   000a   0004   0004   ffffffff
2202   0004   0004   0004   00000000
2216   0004   000a   0024   ffffffff
2382   000a   000a   0024   00000000
```
Fired block colour equalled the +0x90 value at the preceding queue change for 5 of 5 shots (shots at frames
about 2002, 2206, 2386, 2520, 2872; queue changes at 1870 (4,a), 1998 (a,a), 2066 (a,4), 2202 (4,4), 2216
(4,a), 2382 (a,a)). Block colour changed in place at the same PPPP: P1 (9,4) `008a317c` -> `0084317c` at frame
2052; (7,3) and (7,4) `0x84` -> `0x8a` at frame 2922 (run B).

Type byte histogram over gameplay frames 1750..3650 (run A/B dumps): P1 {0x84, 0x86, 0x8a, 0x8e}; P2 {0x92,
0x94, 0x96, 0x9a, 0x9e, 0xb0}. Byte0 = 0xff on whole rows from row 16 upward in steps of two rows (run A, P1, dump
every 50 frames): no such row at frame 2950; rows 15-16 at 3000..3200; rows 13-16 at 3250..3650. P2 had none
in any dump from 2800 to 3650.

Screenshot reading (`rendered.png`, run A frame 3000): row 17 appeared at the top of the screen; code 4 looked
orange and code 10 yellow. This is one screenshot match, not a measured fact.

## Not found / not tested

- No writer of byte0 = 0xff was located. No `mametap write 0x407596:0x4075d5` run was made.
- P2 rows 18 and above were not read: 0x4078b6.. overlaps character-select state 0x4078f0..0x40791f.
- The 8x/9x player split and the 0xb0 specials come from one 1P-vs-CPU run; no 2P versus run was made.
