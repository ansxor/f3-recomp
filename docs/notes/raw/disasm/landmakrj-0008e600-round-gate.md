---
source_url: "tools/f3a dis --game landmakrj 0x08e62e --from 0x08e600 --to 0x08e640"
ingested: 2026-10-09
sha256: f4248ffaccea636ed992ec676f404ebb09f7b243e4ed120bdba798f7575c99f6
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "tools/f3a (git 47cbdc3 + uncommitted)"
---
```
sub_08e20c @ 0x08e20c  kinds=ref,seed  insns=404  span=0x08e20c..0x08e84a  caller_sites=1  entry_hits=10 max_block_hits=86223
  0x08e62e is not a routine entry: it is code inside sub_08e20c; `f3a dis 0x08e62e --count N` reads just that code
  only 0x08e600..0x08e640 (17 instructions)
  0x08e602         x4  bset.b #$5, -$60ad(a5)                    ; rw 0x401f53 ram (a5-$60ad)
  0x08e608         x4  bsr.w $8f650                              ; → sub_08f650
  0x08e60c         x4  bra.b $8e636                            
  0x08e60e        x15  clr.w -$60a4(a5)                          ; w 0x401f5c ram (a5-$60a4)
  0x08e612        x15  bsr.w $8f650                              ; → sub_08f650
  0x08e616        x15  tst.w -$609e(a5)                          ; r 0x401f62 ram (a5-$609e)
  0x08e61a        x15  bne.b $8e626                            
  0x08e61c        x13  cmpi.w #$a, -$60ac(a5)                    ; r 0x401f54 ram (a5-$60ac)
  0x08e622        x13  blt.b $8e636                            
  0x08e624     rooted  bra.b $8e62e                            
  0x08e626         x2  cmpi.w #$3, -$60ac(a5)                    ; r 0x401f54 ram (a5-$60ac)
  0x08e62c         x2  blt.b $8e636                            
  0x08e62e     rooted  bsr.w $8e8b8                              ; → sub_08e8b8
  0x08e632     rooted  bra.w $8e6d2                            
  0x08e636        x19  btst.b #$3, -$60c0(a5)                    ; r 0x401f40 ram (a5-$60c0)
  0x08e63c        x19  beq.w $8e464                            
  0x08e640         x7  tst.b -$7f2e(a5)                          ; r 0x4000d2 ram (a5-$7f2e)
```
