---
source_url: "tools/f3a dis --game landmakrj (sub_08edde, sub_08ee06, sub_08f0d6, sub_08f79a)"
ingested: 2026-10-10
sha256: a2d62c9e01118c99a188ac309c66cadf6edc40bd23c7a69f1c8c2d909489fad7
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "tools/f3a (git 849cfef)"
---
# Versus latch and player join routines in landmakrj

## sub_08edde: Mode select completion and 2P VS latch store
```
sub_08edde @ 0x08edde  kinds=call,task  insns=11  span=0x08edde..0x08ee04  caller_sites=1  entry_hits=10 max_block_hits=1830
> 0x08edde        x10  move.l #$c, -(a7)                       
  0x08ede4        x10  pea.l $9e962.l                            ; code ref → sub_09e962; & 0x09e962 rom code
  0x08edea        x10  trap #$1                                
  0x08edec        x10  addq.l #$8, a7                          
  0x08edee      x1830  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08edf0
  0x08edf0      x1830  btst.b #$c, -$7c86(a5)                    ; r 0x40037a ram (a5-$7c86)
  0x08edf6      x1830  bne.b $8edee                            
  0x08edf8        x10  tst.w -$609a(a5)                          ; r 0x401f66 ram (a5-$609a)
  0x08edfc        x10  beq.b $8ee04                            
  0x08edfe         x5  move.w #$1, -$6092(a5)                    ; w 0x401f6e ram (a5-$6092)
  0x08ee04        x10  rts                                     
```

## sub_08ee06: Character select loop, mid-charsel join check, and 2P VS latch store
```
sub_08ee06 @ 0x08ee06  kinds=call,task  insns=17  span=0x08ee06..0x08ee44  caller_sites=1  entry_hits=10 max_block_hits=8200
> 0x08ee06        x10  move.l #$c, -(a7)                       
  0x08ee0c        x10  pea.l $9cd04.l                            ; code ref → sub_09cd04; & 0x09cd04 rom code
  0x08ee12        x10  trap #$1                                
  0x08ee14        x10  addq.l #$8, a7                          
  0x08ee16      x8200  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08ee18
  0x08ee18      x8200  tst.w -$6f0(a5)                           ; r 0x407910 ram (a5-$6f0)
  0x08ee1c      x8200  bne.b $8ee3c                            
  0x08ee1e      x7870  bsr.w $8f398                              ; → sub_08f398
  0x08ee22      x7870  tst.w -$6082(a5)                          ; r 0x401f7e ram (a5-$6082)
  0x08ee26      x7870  beq.b $8ee3c                            
  0x08ee28     rooted  clr.w -$6082(a5)                          ; w 0x401f7e ram (a5-$6082)
  0x08ee2c     rooted  move.w #$1, -$6092(a5)                    ; w 0x401f6e ram (a5-$6092)
  0x08ee32     rooted  move.w -$609e(a5), -$609c(a5)             ; r 0x401f62 ram (a5-$609e); w 0x401f64 ram (a5-$609c)
  0x08ee38     rooted  clr.w -$609e(a5)                          ; w 0x401f62 ram (a5-$609e)
  0x08ee3c      x8200  btst.b #$c, -$7c86(a5)                    ; r 0x40037a ram (a5-$7c86)
  0x08ee42      x8200  bne.b $8ee16                            
  0x08ee44        x10  rts                                     
```

## sub_08f0d6: Player start join handler (sets 0x401f53 and 0x401f7e)
```
sub_08f0d6 @ 0x08f0d6  kinds=call  insns=45  span=0x08f0d6..0x08f164  caller_sites=2  entry_hits=20646 max_block_hits=25400
  shares code with sub_08f00a (1 insns) (branches into common tails)
> 0x08f0d6     x20646  bsr.w $8f2de                              ; → sub_08f2de
  0x08f0da     x20646  tst.w d3                                
  0x08f0dc     x20646  blt.w $8f164                            
  0x08f0e0     rooted  bclr.b #$3, -$60c0(a5)                    ; rw 0x401f40 ram (a5-$60c0)
  0x08f0e6     rooted  bset.b d3, -$60ad(a5)                     ; rw 0x401f53 ram (a5-$60ad)
  0x08f0ea     rooted  move.w d3, -$6084(a5)                     ; w 0x401f7c ram (a5-$6084)
  0x08f0ee     rooted  move.w d3, d0                           
  0x08f0f0     rooted  addq.w #$6, d0                          
  0x08f0f2     rooted  bset.b d0, -$60ad(a5)                     ; rw 0x401f53 ram (a5-$60ad)
  0x08f0f6     rooted  move.w #$1, -$6082(a5)                    ; w 0x401f7e ram (a5-$6082)
  0x08f0fc     rooted  tst.b -$7f2d(a5)                          ; r 0x4000d3 ram (a5-$7f2d)
  0x08f100     rooted  bne.b $8f164                            
  0x08f102     rooted  addq.w #$2, d3                          
  0x08f104     rooted  move.w d2, d0                           
  0x08f106     rooted  addq.w #$2, d0                          
  0x08f108     rooted  btst.l d0, -$60ad(a5)                     ; r 0x401f53 ram (a5-$60ad)
  0x08f10c     rooted  bne.b $8f120                            
  0x08f10e     rooted  move.w -$60aa(a5), d0                     ; r 0x401f56 ram (a5-$60aa)
  0x08f112     rooted  addq.w #$1, d0                          
  0x08f114     rooted  move.w d0, -(a7)                        
  0x08f116     rooted  move.b d2, -(a7)                        
  0x08f118     rooted  jsr $49e.l                                ; → sub_00049e; & 0x00049e rom code
  0x08f11e     rooted  addq.l #$4, a7                          
  0x08f120     rooted  btst.l d3, -$60ad(a5)                     ; r 0x401f53 ram (a5-$60ad)
  0x08f124     rooted  bne.b $8f12e                            
  0x08f126     rooted  jsr $b1e.l                                ; → sub_000b1e; & 0x000b1e rom code
  0x08f12c     rooted  rts                                     
  0x08f12e     rooted  jsr $b24.l                                ; → sub_000b24; & 0x000b24 rom code
  0x08f134     rooted  clr.w -$60bc(a5)                          ; w 0x401f44 ram (a5-$60bc)
  0x08f138     rooted  move.w -$60aa(a5), d0                     ; r 0x401f56 ram (a5-$60aa)
  0x08f13c     rooted  addq.w #$1, d0                          
  0x08f13e     rooted  move.w d0, -(a7)                        
  0x08f140     rooted  move.b d2, -(a7)                        
  0x08f142     rooted  jsr $46a.l                                ; → sub_00046a; & 0x00046a rom code
  0x08f148     rooted  addq.l #$4, a7                          
  0x08f14a     rooted  tst.w -$609a(a5)                          ; r 0x401f66 ram (a5-$609a)
  0x08f14e     rooted  beq.b $8f164                            
  0x08f150     rooted  tst.w d2                                
  0x08f152     rooted  seq.b d1                                
  0x08f154     rooted  andi.w #$1, d1                          
  0x08f158     rooted  move.w d0, -(a7)                        
  0x08f15a     rooted  move.b d1, -(a7)                        
  0x08f15c     rooted  jsr $46a.l                                ; → sub_00046a; & 0x00046a rom code
  0x08f162     rooted  addq.l #$4, a7                          
  0x08f164     x25400  rts                                     
```

## sub_08f79a: Mid-game challenger sequence ("HERE COMES A NEW CHALLENGER")
```
sub_08f79a @ 0x08f79a  kinds=call,task  insns=62  span=0x08f79a..0x08f892  caller_sites=2  entry_hits=86221 max_block_hits=86221
> 0x08f79a     x86221  tst.w -$6082(a5)                          ; r 0x401f7e ram (a5-$6082)
  0x08f79e     x86221  beq.w $8f88e                            
  0x08f7a2     rooted  move.l -$60b0(a5), d0                     ; r 0x401f50 ram (a5-$60b0)
  0x08f7a6     rooted  andi.w #$3, d0                          
  0x08f7aa     rooted  cmpi.w #$3, d0                          
  0x08f7ae     rooted  bne.w $8f88e                            
  0x08f7b2     rooted  move.w -$609e(a5), -$609c(a5)             ; r 0x401f62 ram (a5-$609e); w 0x401f64 ram (a5-$609c)
  0x08f7b8     rooted  clr.w -$609e(a5)                          ; w 0x401f62 ram (a5-$609e)
  0x08f7bc     rooted  move.l #$1000, -(a7)                    
  0x08f7c2     rooted  trap #$2                                
  0x08f7c4     rooted  addq.l #$4, a7                          
  0x08f7c6     rooted  move.w #$77, d7                         
  0x08f7ca     rooted  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08f7cc
  0x08f7cc     rooted  move.w #$e, -(a7)                       
  0x08f7d0     rooted  pea.l $61c78e.l                           ; & 0x61c78e text
  0x08f7d6     rooted  pea.l $8fba6(pc)                          ; & 0x08fba6 rom data
  0x08f7da     rooted  nop                                     
  0x08f7dc     rooted  jsr $57a2.l                               ; → sub_0057a2; & 0x0057a2 rom code
  0x08f7e2     rooted  lea.l $a(a7), a7                        
  0x08f7e6     rooted  dbra d7, $8f7ca                         
  0x08f7ea     rooted  clr.w -$6082(a5)                          ; w 0x401f7e ram (a5-$6082)
  0x08f7ee     rooted  move.w #$1, -$6092(a5)                    ; w 0x401f6e ram (a5-$6092)
  0x08f7f4     rooted  clr.w -$6098(a5)                          ; w 0x401f68 ram (a5-$6098)
  0x08f7f8     rooted  clr.w -$6096(a5)                          ; w 0x401f6a ram (a5-$6096)
  0x08f7fc     rooted  clr.w -$6094(a5)                          ; w 0x401f6c ram (a5-$6094)
  0x08f800     rooted  jsr $59ee.w                               ; → sub_0059ee; & 0x0059ee rom code
  0x08f804     rooted  move.l #$a, -(a7)                       
  0x08f80a     rooted  pea.l $8e096.l                            ; code ref → sub_08e096; & 0x08e096 rom code
  0x08f810     rooted  trap #$1                                
  0x08f812     rooted  addq.l #$8, a7                          
  0x08f814     rooted  move.w #$1f, d0                         
  0x08f818     rooted  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08f81a
  0x08f81a     rooted  dbra d0, $8f818                         
  0x08f81e     rooted  bsr.w $8ed8a                              ; → sub_08ed8a
  0x08f822     rooted  jsr $c10.l                                ; → sub_000c10; & 0x000c10 rom code
  0x08f828     rooted  jsr $41d0.w                               ; → sub_0041d0; & 0x0041d0 rom code
  0x08f82c     rooted  jsr $5b64.w                               ; → sub_005b64; & 0x005b64 rom code
  0x08f830     rooted  jsr $5cd8.w                               ; → sub_005cd8; & 0x005cd8 rom code
  0x08f834     rooted  jsr $3352.l                               ; → sub_003352; & 0x003352 rom code
  0x08f83a     rooted  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08f83c
  0x08f83c     rooted  clr.l -$6054(a5)                          ; w 0x401fac ram (a5-$6054)
  0x08f840     rooted  clr.l -$6050(a5)                          ; w 0x401fb0 ram (a5-$6050)
  0x08f844     rooted  ori.w #$1, -$604a(a5)                     ; rw 0x401fb6 ram (a5-$604a)
  0x08f84a     rooted  clr.l -$5fac(a5)                          ; w 0x402054 ram (a5-$5fac)
  0x08f84e     rooted  clr.l -$5fa8(a5)                          ; w 0x402058 ram (a5-$5fa8)
  0x08f852     rooted  ori.w #$1, -$5fa2(a5)                     ; rw 0x40205e ram (a5-$5fa2)
  0x08f858     rooted  move.w #$4e, -(a7)                      
  0x08f85c     rooted  jsr $308c.l                               ; → sub_00308c; & 0x00308c rom code
  0x08f862     rooted  lea.l $2(a7), a7                        
  0x08f866     rooted  move.w #$4e, -$607e(a5)                   ; w 0x401f82 ram (a5-$607e)
  0x08f86c     rooted  move.l #$c, -(a7)                       
  0x08f872     rooted  pea.l $9cd04.l                            ; code ref → sub_09cd04; & 0x09cd04 rom code
  0x08f878     rooted  trap #$1                                
  0x08f87a     rooted  addq.l #$8, a7                          
  0x08f87c     rooted  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08f87e
  0x08f87e     rooted  btst.b #$c, -$7c86(a5)                    ; r 0x40037a ram (a5-$7c86)
  0x08f884     rooted  bne.b $8f87c                            
  0x08f886     rooted  trap #$5                                  ; yield (trap5 task switch): resumes at 0x08f888
  0x08f888     rooted  move.w #$0, d0                          
  0x08f88c     rooted  rts                                     
  0x08f88e     x86221  move.w #$ffff, d0                       
  0x08f892     x86221  rts                                     
```
