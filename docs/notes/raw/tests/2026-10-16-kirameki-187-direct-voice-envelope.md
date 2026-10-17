---
source_url: "user PCB recording /home/answer/Downloads/kirameki-sound-1-187.flac (Kirameki sound test BANK 01 / CODE 0x187, song $2c) vs HLE and reference-backend renders; measurement scripts inline below"
ingested: 2026-10-16
sha256: 8666f8a32cdb89ea7813bf9fba1fd3e8e48e96b6783d411ee5ff53d06496f21d
game: [kirameki]
rom_sha256:
  kirameki: d8ac9f4af1871e9d530407ac1181f5227b9d2badd6111c2198ff5103f19a4aa8
captured_with: "f3rt-tool sound-extract (enhanced + reference/native), capstone 5.0.7 m68k, temporary ES5505 register write trace (reverted)"
---
# Kirameki 0x187: direct-voice envelope timer, cymbal cut, and the declick tail

sha256 above is of the FLAC. PCB is L/R swapped vs our output (swapped back for every number below). Times are seconds
from the sound-extract event origin. PCB aligned by cross-correlation (PCB = HLE + 1.648 s; reference backend = HLE - 0.029 s).

## Findings

1. Root cause is the HLE direct-voice envelope timer expiry, not ES5505 volume ramps. The chip has no ramp in this driver's use:
   the driver writes only LVOL/RVOL (page regs r8/r9, top byte used) every ~1 ms, instantly; MAME's ES5505 (and ours) implements no
   LVRAMP/RVRAMP/ECOUNT behavior and no register trace shows them written. The HLE's 4 ms volume ramp (`VOLUME_RAMP`) is only
   a smoothing between its 12 ms parameter updates.
2. The software ADSR is modelled (rates from RATE_TABLE $c089dc, `update_envelope`), and note-off does start the release
   (`release()` = c19824/c1983c). The error was the expiry dispatch for "direct" voices (c1971c): HLE chose by `v.released`
   (released -> stop at once; not released -> hold forever). The driver dispatches by stage (`(a6)` at `$38(a4)`).
3. The cymbal (key 20, layer 6, direct, patch byte0 bit7 set so note-off is ignored): the driver holds LVOL $da10 for ~150 ms
   (timer, patch +0 rate), then c1983c starts the release ramp at patch +2 rate: LVOL falls ~62 units/ms to $b7xx at ~290 ms and the
   voice ends. HLE (before) hit the timer at 142 ms with `v.released` already set by the ignored note-off, so it stopped at full level
   $da10; the same expiry on an unreleased direct voice held its level forever.
4. The declick tail (commit 480969a, `TAIL_DECAY`, `cut_output`, `mix_tail` in synth.cpp) is NOT the cymbal bug and is not hardware
   behavior. After the fix it was measured against the commit's motivating case and removed (see "Tail verdict").

## Driver excerpts (kirameki sound ROM, e44-20/e44-21 interleaved, base $c00000)

```
$c1971c  moveq #0,d7 ; lea $56(a4),a1 ; move.l (a1)+,d0 ; add.l (a1),d0 ; move.l d0,(a1)+   ; position += slope
$c19728  cmp.l (a1)+,d0 ; bmi.b $c19752                         ; below target: keep ramping
$c1972c  lea $38(a4),a6 ; tst.l -$c(a1) ; bgt.b $c1973c
$c19736  eori.w #$8000,-$4(a1)                                  ; slope<=0: target hi ^= 8000
$c1973c  move.l -$4(a1),-$8(a1)                                 ; position = target (level restored)
$c19742  movea.l #$c1983c,a0 ; adda.w (a6),a0 ; jsr (a0)        ; stage routine at $c1983c + stage
$c1974c  bcc.b $c19752 ; bra.w $c1789a                          ; carry = end voice
; stage 0:
$c1983c  moveq #0,d6 ; move.w d6,-$4(a1) ; move.b $2(a3),d4 ; and.w #$7f,d4 ; add.w d4,d4
$c1984c  movea.l #$c089dc,a0 ; sub.w #$7f00,d6 ; muls.w (a0,d4.w),d6 ; add.l d6,d6   ; slope = -$7f00*rate*2
$c1985c  move.w #0,-6(a1) ; move.w #0,-2(a1) ; move.l d6,-$c(a1) ; tst.l -$c(a1) ; bgt $c19878
$c19872  eori.w #$8000,-$4(a1) ; $c19878 move.w #$4c,(a6)       ; next stage = $4c
$c1987c  tst.b -$8(a1) ; bne.b $c19886 ; ori.b #1,ccr           ; level top byte 0 -> carry (end now)
; stage $4c (c19888): clears slope/target hi, stage=$6c, ori #1,ccr -> carry: voice ends
$c19824  tst.b 0(a3) ; bpl $c1982c ; rts                        ; note-off: patch byte0 bit7 -> ignored
$c1982c  lea $38(a4),a6 ; cmpi.w #$4c,(a6) ; bne $c19838 ; rts  ; already releasing -> ignored
```
Note-off and timer expiry therefore share one routine (c1983c); the stage, not the note-off flag, decides.

## Fix (runtime/audio/hle/synth.cpp)

`start_direct_release()` (c1983c) is used by both `release()` and the direct branch of `update_envelope()`; on timer expiry:
stage 4 or level top byte 0 -> end voice, otherwise start the release ramp (previously: released -> end, else hold).
Test: `games/kirameki/tests/audio.cpp` `HleAudio.KiramekiDirectVoiceTimerStartsRelease` (fails on the old logic, verified);
CMake `hle-audio` filter widened to `HleAudio.*`.

## Measurements

HLE voice i815 (key 20 layer 6), ms after note on; reference = ES5505 LVOL writes from the native-driver reference run:

| t (ms) | ref LVOL | HLE before | HLE after |
|---|---|---|---|
| 0-142 | $da10 | $da10 | $da10 |
| 153 | $d9e1 | (stopped at 142) | $d710 (12 ms update granularity) |
| 201 | $cf30 | - | $cb40 |
| 285-293 | $b8f5 / $b733 | - | $b670 |
| voice end | ~293 (stolen/next note 317) | 142 | 294 |

Whole-song check around the silence at 20-21 s (100 ms RMS, dB; PCB row = at t+1.648):

| t | 19.0 | 19.5 | 20.0 | 20.2 | 20.4 | 20.6 | 20.8 | 21.0 |
|---|---|---|---|---|---|---|---|---|
| PCB | -23.0 | -20.5 | -45.0 | -50.0 | -55.4 | -60.3 | -63.9 | -68.7 |
| reference | -30.0 | -27.3 | -55.4 | -62.2 | -68.4 | -69.1 | -67.8 | -66.4 |
| HLE before | -31.8 | -28.7 | -47.9 | -47.6 | -47.6 | -47.7 | -47.2 | -46.2 |
| HLE after | -31.8 | -28.4 | -65.4 | -84.2 | -99.0 | -122.8 | -140.0 | -140.0 |

Before, a direct voice held forever (flat -47.6 dB, stuck). Reference ES5505 trace: every voice's LVOL reaches ~$0fe5 (silent) and ends by 20.71 s,
matching HLE after (last stop 20.764 s). The PCB/reference remaining slow decline (-45 -> -83 dB) is the ESP reverb tail, which HLE lacks
here (effects/output mix owned by another change); it is not voice level.

Cymbal band (4-12 kHz, 10 ms) first hit, HLE vs PCB: shape matches already (-46.6 -> -84 dB over 150 ms vs PCB -42.8 -> -83); the first hit
is the key-43 layer-8 voice, which was correct before and after (stop 235.7 ms vs reference ~240 ms, sample end). Constant HLE offset
vs PCB ~7-8 dB is the output stage (other worker).

## Tail verdict (declick tail removed)

Motivating case (commit 480969a: "clicking at the end of bass drums", regression check 6 of landmakrj `HleAudio.StateAndSequencing`):
landmakrj direct SFX / sequence 0x87 (packets `038101`, `068d01014002`, `038187`, `--wav-window event`, 20 s). Rendered after the fix
with the tail, with the tail disabled (temporary `NO_TAIL` hack, reverted), and with the reference backend (native driver, instant stop).
Event origin cycles=244300326; reference aligned to HLE per stop by local cross-correlation (+-30 ms). Metric: largest sample step within +-1.5 ms
of the Stop vs RMS step in the preceding 30 ms; stops with LVOL >= $4000 (213 of 644 stops; 91 above $8000), no other start/stop/release
within +-6 ms (40 stops, mostly LVOL $7320, others $a7d0-$c1b0):

| | HLE + tail | HLE no tail | reference |
|---|---|---|---|
| mean step/local | 2.88 | 2.88 | 2.94 |
| stops > 4x | 4 | 4 | 1 |
| max abs difference tail vs no tail, 4 ms after the cut | 15 LSB | | |

(Without the isolation filter, 72 of 86 "big step" stops coincide with a note start on the same tick, which confounds the metric: tail 6.71,
no tail 6.71, reference 3.57. Tail and no-tail are identical there too, so the tail does nothing at these cuts either.) Kirameki 0x187, 71 clusters
where tail and no-tail differ by >60 LSB: mean step ratio no tail 3.80, tail 3.72, reference 5.84, PCB 5.71 (HLE is smoother than hardware).
Conclusion: HLE without the tail has no more high-level cuts than the reference backend or the PCB (the extra 3 stops above 4x are identical with and without the tail,
so are not tail-preventable), and the tail is not hardware. Removed: `TAIL_DECAY`, `cut_output`, `mix_tail`, `inject[]`, per-voice last-output
state, `lane_end`/`lane_prev_*`, and landmakrj test check 6 (it asserted decay-after-Stop). [INFERENCE] The clicks heard at the time may have been voices
stopping at full level (the bug class fixed above); this was not reproduced on the original machine.

## Reproduction

```
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --out-dir OUT --hle-events OUT/ev.csv
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --audio-backend reference --sound-driver native --out-dir REF
ffmpeg -i /home/answer/Downloads/kirameki-sound-1-187.flac pcb.wav     # then swap channels, shift +1.648 s
```
Register trace (temporary, reverted): in `ES5505::write` print `ESW <chip sample#> v<page> r<reg> <data>/<mask>` for pages < 0x20, with a counter
incremented in `generate_one_sample`; chip sample n -> seconds (n - 454413) / 29762 - 0.2755 aligns to HLE time. Disassembly: interleave
e44-20.51 (even bytes) and e44-21.52 (odd bytes), capstone `CS_ARCH_M68K`/`CS_MODE_M68K_020` from file offset (addr - 0xc00000);
locate RATE_TABLE users by searching for the longword `00c089dc` ($c13008, $c1831a, $c18ed2, $c19388, $c1984e). HLE event CSV kinds:
0 start, 1 release, 2 stop, 3 parameters; columns tick (16 MHz), left volume = column 12.
Landmakrj tail case: `f3rt-tool sound-extract --rom-dir roms/landmakrj --seconds 20 --wav-window event --packet 038101 --packet 068d01014002 --packet 038187 --wav X.wav [--audio-backend enhanced --hle-events E.csv | --audio-backend reference --sound-driver native]`.
Metric script (band envelopes, step ratios): docs/notes/raw/tests/2026-10-16-kirameki-187-pcb-metrics.py.txt.
