---
source_url: "f3rt landmakr (origin/main 5be4058, --audio-backend reference, native sound driver) vs f3rt landmakrj (HLE) driven through the Land Maker test menu; Capstone M68K disassembly of the sound ROM"
ingested: 2026-11-05
sha256: unrecorded (WAVs /tmp/s_ref.wav, /tmp/r_*.wav, /tmp/h2_*.wav)
game: [landmakrj]
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "f3rt landmakr / landmakrj --headless --unthrottled --fast-boot off --inputs <script> --wav; per-10 s RMS in Python; capstone 5.0.7"
---
# Land Maker songs: which sequences loop on the reference driver

Question: sound-test 0x56 (and gameplay music) ends after one pass in the HLE. Does the sound driver repeat it?

## Method

Same input script on both backends (reference = the real sound-CPU code, native recompiled driver). `--inputs`:
`1000-1010 test` opens the test menu, two `down` and `b1` enter SOUND TEST, then `right` presses raise SOUND NO (shown
in hex, starts at 001; code N needs N-1 presses) and `b1` plays it. The sound-test selector table at main `$6bde` gives
the song: record `[0, song, volume]`, packets `03 81 song` then `04 86 song volume`. Level = RMS per 10 s of the WAV,
from 30 s on (the songs start at 32-34 s of game time, depending on the number of presses); times in the table are absolute game time.

## Result (RMS per 10 s window, 30 s to 150 s)

| Code | Song | Header flags (+1) | Reference | HLE before the change |
|---|---|---|---|---|
| 0x56 | `$0f` | `0x12` | keeps playing; the quiet passage at 80-90 s recurs at 145-150 s (period about 65.7 s = 32 bars) | silent from about 98 s (song starts at 33.7 s) |
| 0x4e | `$03` | `0x12` | keeps playing | silent from about 81 s |
| 0x58 | `$13` | `0x12` | keeps playing | silent from about 100 s |
| 0x59 | `$14` | `0x12` | repeats every about 40 s | silent from about 80 s |
| 0x5a | `$15` | `0x12` | keeps playing | silent from about 110 s |
| 0x5d | `$1a` | `0x12` | keeps playing | silent from about 50 s |
| 0x53 | `$0a` | `0x08` | ends at about 66 s, 34.5 s after the start (noise floor 20 RMS) | same |
| 0x54, 0x57, 0x55, 0x5b | `$0b`, `$92`, `$8e`, `$98` | `0x08` / arranged (bit 7 selects a parent) | still playing at 120 s | same |

After the HLE change (`SEQ_FLAG_LOOP`, runtime/audio/hle/sequencer.cpp) the HLE timelines of 0x56, 0x4e, 0x58,
0x59, 0x5a and 0x5d keep playing and follow the reference windows (0x59: 606/1030/1269/1308/1030/1030/1269/1310
against 618/1015/1226/1275/1040/1014/1227/1274); 0x53 still ends. 0x58 is about 18% quieter than the reference in its
first 20 s before and after the change (653 vs 795 in window 1); unexplained.

Header flags of the other songs in the bank (byte `+1`): `0x04 0x10`, `0x09 0x12`, `0x0c 0x10`, `0x0d 0x12`, `0x0e 0x12`
(bars 0, arrangement parent), `0x10 0x10`, `0x11 0x12`, `0x12 0x12` (parent), `0x13 0x12`, `0x14 0x12`, `0x15 0x12`,
`0x16 0x10`, `0x17 0x12`, `0x18 0x12` (parent), `0x19 0x12`, `0x1a 0x12`; `0x0a` and `0x0b` carry `0x08` (and a
nonzero loop pair at `+0x16/+0x1a`). Flags `0x12` and `0x10` differ only in bit 1, so bit 1 is the discriminator.
Not established: the driver code that acts on it. The only header-flag users found are `btst #0` (`$c15732`, `$c1572c`,
`$c158f0`) and a getter/setter pair for bit 1 (`$c15948`, `$c1591c`) whose caller was not followed.

## Driver excerpts read while looking for the loop

Bar-boundary tail of the tick routine (sound image, interleaved lanes e61-14.32 / e61-15.33):

```
c14d0a  move.w $d0ba.w, d0        ; header bar count
c14d0e  cmp.w  $d0b0.w, d0        ; bars played
c14d12  bne.w  $c14da4            ; not at the boundary
c14d16  clr.w  $d0b0.w
c14d1a  move.w $d40e.w, d0        ; current sequence
c14d1e  bsr.w  $c12a46            ; slot $5e5c + 0x28 * id
c14d22  move.b $24(a0), d0        ; slot byte +$24 (arrangement child id, set at $c12d7e)
c14d26  beq.b  $c14d30
c14d28  bsr.w  $c12b9e            ; arrangement repeat / advance
c14d2c  bsr.w  $c13db0            ; restart tracks
c14d30  bsr.w  $c158f0            ; header +1 bit 0 -> Z
c14d34  beq.b  $c14d8c
c14d8c  clr.l  $d0d0.w
c14d90  move.w $d0f0.w, d0        ; queued next selection, $ffff = none
c14d94  bmi.b  $c14da4
c14d96  bsr.b  $c14dc8
c14d98  bsr.w  $c155e8            ; start sequence d0
c14d9c  bsr.w  $c13db0
c14da0  bsr.w  $c155ba
```

Start commands:

```
c12ce0  clr.b  $6e1a.w            ; opcode 0x80
c12ce6  tas.b  $6e1a.w            ; opcode 0x81
c12d86  tst.b  $6e1a.w
c12d8a  bne.b  $c12d90
c12d8c  clr.w  $d524.w            ; only 0x80 clears $d524 (set at $c15406 from a clamped scaled value)
```

So the 0x80/0x81 difference in the driver is whether `$d524` survives; nothing in it makes 0x81 loop.
`$6e19` is only written as the low byte of the word `$6e18` (`$c12d44`, `$c12cee`).

## Reproduction

```sh
# disassembly helper: capstone M68K, skipdata, image[0::2] = e61-14.32, image[1::2] = e61-15.33, address - 0xc00000
git worktree add /tmp/ref origin/main && cmake -S /tmp/ref -B /tmp/ref/build -DF3_GAME=landmakrj -DF3_ROM_DIR=roms/landmakrj
ninja -C /tmp/ref/build landmakr
# input script for code N (python): test at 1000, down x2, b1, then N-1 'right' presses 6 frames apart, then b1
landmakr --rom-dir roms/landmakrj --headless --no-audio --unthrottled --fast-boot off --frames F --inputs in.txt \
         --audio-backend reference --wav ref.wav
landmakrj --rom-dir roms/landmakrj --headless --no-audio --unthrottled --fast-boot off --frames F --inputs in.txt --wav hle.wav
```

The HLE PCM ring (32768 frames) overflows in an unthrottled headless run once the emulation outruns the synth worker; the
HLE runs above used a temporary 2^24-frame ring.
