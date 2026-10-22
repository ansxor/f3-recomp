---
source_url: user-supplied PCB recordings /home/answer/Downloads/kirameki-sound-1-187.flac (Kirameki Star Road sound test BANK 01 / SOUND CODE 0x187, BGM song $2c) and /home/answer/Downloads/kirameki-sound-1-1.wav (bank 1, sound code 1, recorded from cold boot by 12; the blip at the very start is a recording artifact); runtime renders; MAME 0.289 esqpump.cpp (https://raw.githubusercontent.com/mamedev/mame/mame0289/src/devices/sound/esqpump.cpp); docs/notes/raw/docs/taito-f3/5505-5510.txt at submodule revision 9927deb
ingested: 2026-10-16
sha256: 8666f8a32cdb89ea7813bf9fba1fd3e8e48e96b6783d411ee5ff53d06496f21d
game: [kirameki]
captured_with: "f3rt-tool sound-extract (HLE default; --audio-backend reference --sound-driver native), scripts and diagnostic patch description in 2026-10-16-kirameki-187-pcb-metrics.py.txt; sha256 above is of kirameki-sound-1-187.flac (kirameki-sound-1-1.wav was not hashed)"
---
# Kirameki PCB recordings vs the MAME-derived emulation and HLE: level, channel order, reverb

**Target = the two PCB recordings.** "MAME-derived emulation" is the runtime's reference backend (real 68000 driver + MAME-derived ES5505/ES5510); it is not ground truth and its deviation from the PCB is itself a finding. All numbers below are errors against the PCB unless stated.

Signals are high-passed at 20 Hz (the emulated ESP output carries a constant DC offset of about -26/-23 LSB), resampled to 29762 Hz, and the tail is measured in 50 ms windows after the song end relative to the RMS of the 2 s before the end. Recording A = 0x187 (song ends at 21.5 s in the file; preset 5), recording B = sound code 1 (song 8.55..16.15 s in the file, tail until about 17.3 s, then it loops). The recordings' L/R wiring and capture chain are unknown. 12's oscilloscope tracing is in `docs/notes/raw/docs/taito-f3/5505-5510.txt`.

Reproduction:

```
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 24 --tail 4 --out-dir OUT/hle_A
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 24 --tail 4 \
    --audio-backend reference --sound-driver native --out-dir OUT/ref_A      # same with --sound 1 --seconds 12 for B
uv run --group analysis python final_tab.py     # tables below (scripts in 2026-10-16-kirameki-187-pcb-metrics.py.txt)
```

## 1. Routing: MAME order is correct; the earlier pair 1/3 exchange is withdrawn

`5505-5510.txt` (oscilloscope, authoritative): OTIS ser0 -> nc; ser1 -> ESP serio0; ser2 -> serio1; ser3 -> serio2; ESP serio3 -> DAC. This is MAME's esqpump order (pump inputs 2..7 -> `ser_w(0..5)`, output `ser_r(6/7)`):

```
stream.put(2, 0, stream.get(0, 0));  stream.put(3, 0, stream.get(1, 0));   // aux bypass of OTIS pair 0
m_esp->ser_w(0, stream.get(2,0)*32768); ... m_esp->ser_w(5, stream.get(7,0)*32768);
l = m_esp->ser_r(6)/32768; r = m_esp->ser_r(7)/32768;
```

An earlier hypothesis (OTIS pairs 1 and 3 exchanged, from the first 12 relay `otis-esp-dac-routing-2026-10-09.txt`, whose confusion arose from the Puzzle Bobble 2 manual's ambiguous "S" connector pin diagram) improved the 0x187 end-of-song tail (mean |err| 3.3 dB, tail +3.2 dB high, vs 5.3 dB low in MAME order; side/mid -7.30 dB vs PCB -7.60), but it contradicts the oscilloscope tracing and worsens the second recording (sound code 1: 6.0 dB low vs 5.3 dB low; the two recordings disagree in sign), so it was reverted. A search over all 6 permutations of OTIS pairs 1..3 onto the ESP inputs x 8 left/right swaps of those inputs (96 reference renders, both recordings) found no routing that matches both recordings (best combined cost 10.4 against 15.4 for MAME order; no row matches channel correlation and tail on both), so routing is not the explanation of the remaining reverb difference below.

Diagnostic (MAME-derived emulation, MAME order, impulse of 0.12 FS on the left input of one OTIS pair, all else muted, preset 5 at the 0x187 settings; energy decay curve re total energy at .01/.05/.1/.2/.3/.5/.75/1 s):

| input (OTIS pair -> ESP) | role seen | EDC dB | left->right leakage |
|---|---|---|---|
| pair 1 -> ser0 | dry: one sample, no tail | none | none |
| pair 2 -> ser1 | delay/echo, first echo 8.9 ms then dense tail | -8.6 -10.0 -10.7 -12.7 -14.2 -17.1 -21.3 -25.0 | L/R energy -10.5 dB |
| pair 3 -> ser2 | reverb, long tail | -9.1 -10.6 -11.3 -13.4 -14.9 -17.8 -22.0 -25.7 | L/R energy -11.1 dB |
| HLE before (pair 2 / pair 3) | echo then silence by 0.2 s / tail gone by 0.2 s | -3.6 -3.7 -26.1 -47.1 / -11.6 -14.5 -28.3 -48.4 | none |

(Diagnostic only: it shows what the driver program does with the PCB-confirmed routing; HLE was not fitted to these curves.) The 0x187 impulse experiments were run in sound-code mode; the ESP/effects state resets when the song stops, so only the first ~1.5 s of tails are meaningful.

## 2. Level: MAME's 0.5 pump route and pair-0 bypass are not on the board

The reference engine computed `(ESP + pair0) * 0.5 * gain`: pair 0 does not reach the DAC (nc) and ESP serio3 feeds the DAC directly, so the output is the ESP result alone. Removing both is the only change needed for level: song RMS error vs PCB:

| render | A (0x187) RMS err | B (code 1) RMS err |
|---|---|---|
| MAME-derived emulation, before (0.5 route + pair-0 bypass) | -7.1 dB | -6.0 dB |
| MAME-derived emulation, after | -1.0 dB | 0.0 dB |
| HLE, after (the same 0.5 removed in `Effects::mix`) | -2.2 dB | -0.1 dB |
| HLE, final (with the decay/damping mapping of section 4) | -2.1 dB | -0.3 dB |

RMS over the last 2 s before the song end (high-passed); error = render minus PCB.
(Absolute: PCB -18.9 / -29.4 dBFS in the same window.) Pair 0 is silent in this song (reference render with only pair 0 live is silent), so the bypass removal is not exercised by the recordings; it follows from the nc wiring. Other candidates checked: OTIS->ESP input gain 0.18 vs unity (the ESP is linear here: level identical, tail within 0.5 dB to 0.6 s and at most 3.7 dB lower at 1.2 s), int16 wrap at the ESP input (not reachable: input clamped to +-1 then x0.18), MB87078 gain applied once (SingleStage default) - none changes level.

## 3. Left/right order

Per-octave L/R energy balance (alignment-free, 100 Hz-12 kHz, 5 s of song; recording is the target; "as-is" = after `Audio::render` exchanges L and R, "not exchanged" = the OTIS/ESP order):

| recording | PCB L/R dB per octave band (200..12k) | as-is: RMS error | not exchanged: RMS error |
|---|---|---|---|
| A (0x187) | -0.2 0.4 -3.7 -2.5 0.6 -0.1 0.7 | MAME-derived 0.27 dB, HLE 0.51 | 3.65 dB, 3.75 |
| B (code 1) | 1.0 0.2 1.0 0.7 0.8 0.6 0.5 | 0.67, 0.67 | 0.97, 1.02 |

A is decisive for the exchanged order (the 400-1600 Hz octaves sit 2.5-3.7 dB lower on the PCB's left and the exchanged output reproduces that to 0.3 dB). B is nearly centred, only marginally favours the exchanged order, and its waveform could not be lag-aligned against ours (correlation 0.1 for either order), so B neither confirms nor refutes. Earlier windowed waveform correlation on A: the exchanged order was better in 14/17 one-second windows for the emulation and 13/17 for HLE before the change, 3/17 and 4/17 after.

What the updated upstream file says (submodule revision 9927deb, last paragraph): "the DAC expects actual I2S input ... it expects I2S left/right meanings, so its output would seem to be backwards from the internal channels. I don't have confirmation of the pinout of the stereo audio output connector (labelled "S") so i can't confirm whether the final output uses the otis/esp ordering or the dac ordering. but probably it is the former, based on hardware recordings i have heard (assuming their stereo was connected and recorded correctly) and based on mame." The previous revision (d95fddf) said the channels are not swapped again after the DAC (so OTIS L/R appear backwards at the output). So the file no longer concludes an exchange; the runtime keeps the exchange (`Audio::render` / `render_ready`, one place, both backends) on the strength of recording A alone; who wired/recorded A is unknown. Decision pending with the user.

## 4. Reverb tail and channel correlation (PCB is the target)

Tail = level in 50 ms windows at +0.05, .15, .25 ... s after the song end relative to the preceding 2 s; "mean |err|" = mean absolute difference to the PCB over 12 points; LRcorr = L/R correlation over the 2 s before the end.

| | A: tail dB (+0.05 .. +1.15 s) | A mean abs err | A LRcorr | B: tail dB | B mean abs err | B LRcorr |
|---|---|---|---|---|---|---|
| **PCB** | -19.0 -26.0 -27.5 -31.0 -34.7 -37.1 -39.4 -44.0 -47.7 -48.3 -51.0 -56.0 | - | 0.699 | -20.1 -21.3 -24.0 -26.6 -30.5 -31.7 -34.5 -37.0 -36.0 -41.1 -43.3 -43.1 | - | 0.539 |
| MAME-derived emulation, before | -23.0 -30.1 -34.1 -37.5 -40.6 -43.9 -46.3 -49.3 -52.9 -55.1 -56.0 -57.3 | 5.4 dB | 0.755 | -20.3 -24.6 -28.3 -31.0 -34.8 -37.5 -41.2 -42.6 -45.7 -48.0 -48.9 -49.9 | 5.3 dB | 0.943 |
| MAME-derived emulation, after | -23.0 -30.1 -34.0 -37.4 -40.5 -43.8 -46.2 -49.3 -52.9 -55.1 -56.1 -57.3 | 5.3 dB | 0.755 | -20.3 -24.6 -28.3 -31.0 -34.8 -37.5 -41.2 -42.6 -45.6 -48.1 -49.1 -50.1 | 5.3 dB | 0.943 |
| HLE, after level/routing fix, formula decay (.52) | -23.9 -33.4 -44.4 -51.9 -58.1 -64.4 -70.7 -78.3 then floor | 23.6 dB | 0.867 | -23.2 -31.8 -40.7 -50.9 -62.1 -73.8 then floor | 31.1 dB | 0.955 |
| HLE, before DSP microcode return fix (decay .92, damping .40) | -21.2 -25.7 -28.6 -29.8 -34.6 -36.1 -39.7 -44.9 -43.3 -47.0 -55.1 -51.9 | 1.8 dB | 0.866 | -19.0 -21.3 -23.6 -28.3 -28.8 -30.7 -32.1 -35.4 -38.2 -41.6 -43.4 -43.8 | 1.1 dB | 0.877 |
| **HLE, final (ES5510 microcode return gain table(127-p8), cross-feed, decorrelation, decay = p0 x (0.91/40))** | -21.3 -24.1 -24.9 -29.8 -34.2 -35.2 -40.2 -40.3 -43.7 -51.6 -50.1 -51.0 | **2.3 dB** | **0.787** | -18.7 -23.0 -27.9 -26.9 -27.7 -31.1 -37.8 -39.2 -39.1 -40.5 -44.2 -51.8 | **2.5 dB** | **0.556** |

Findings:
- The MAME-derived emulation's tail is on average 5.3 dB below the PCB on both recordings (A and B agree to 0.1 dB; the gap opens within the first 0.3 s and the decay slope afterwards is about the PCB's) and its channels are too correlated (0.755 vs 0.699, 0.943 vs 0.539): the PCB is wetter and wider. The cause is not identified. Excluded by measurement: pair routing (section 1), OTIS->ESP input gain, the 0.5/bypass (level only), DC (tails measured after high-pass). Remaining candidates not tested: ES5510 arithmetic/scaling in the emulation (e.g. MAC product shift, saturation/rounding of the reverb loop), which pair the driver assigns to voices in the emulated ES5505 (the HLE and emulation see the same driver commands), and post-DAC analogue effects of the recording chain.
- HLE decay mapping. The heuristic formula (`.3 + p0/180` = .52 for p0 = 40) is far too short against the PCB (tail error 24/31 dB). Kirameki sends the same parameters 0..7 for every song (0x28 0x00 0x14 0x1a 0x25 0x00 0x68 0x40) and only varies 8/9, so the recordings constrain one point. Driver trace (MAME-derived emulation as a diagnostic: every ES5510 host write logged while the preset-5 packets run; parameter changed one at a time, GPR writes diffed): p0 -> GPR110 = p0 << 16 (coefficient p0/128; used in one MAC at instruction 62 of the loaded program, `mac+ g110*g70`), p1 -> GPR134 = p1 << 16, p2 -> GPR133 and GPR107 (delay offsets), p6 -> GPR1/2/118/119, p8 -> GPR124/125, p9 -> GPR122/123, p4 -> several (mode-like), p3/p5/p7 -> none. Discrepancy: in the MAME-derived emulation, changing p0 from 16 to 80 (also p1, p2, p6, p8, p9 individually) leaves the measured impulse response of the delay and reverb inputs identical (tail slope -15.4 dB/s and total energy unchanged to 0.1 dB), so the emulated program shows no p0-dependent decay although the driver writes the coefficient; the ESP-derived shape therefore is only "decay coefficient proportional to p0", and the loop topology that would turn it into a per-pass gain was not recovered. Used: HLE comb decay = p0 x (.92 / 40) clamped to .94 (linear, through the origin, fixed by the PCB point), damping = heuristic (p6 high nibble) x (.40 / .3047) as a 48 kHz-equivalent coefficient (also one PCB point; the p6 -> GPR shape was not derived). Evaluation vs the PCB: tail error 1.8 / 1.1 dB at the 29761 Hz engine rate (1.4 / 1.7 dB at 48 kHz with .90/.25 before section 6); sweeps: decay .86-.94, damping .15-.5.
- Effect on other presets and games (HLE, p0 as sent by the game; the old and new values are decay per comb pass / damping coefficient; there is no PCB evidence for these): landmakrj, preset 11, parameters 30 58 73 32 37 0 40 43 18 32: decay .467 -> .675, damping .055 -> .045. commandw, preset 1, parameters 32 9 40 16 53 0 94 52 58 58 (6 effect selections in 12000 attract frames; the only preset it selected): decay .478 -> .720, damping .219 -> .179. Kirameki preset 5 as above: .522 -> .92 (after the section-6 refit; the other two games' deltas scale by the same .92/.90 factor: landmakrj preset 11 .467 -> .69, commandw preset 1 .478 -> .736). Table defaults in the ROM (identical in all three sound ROMs) for reference, old -> new decay: presets 0/1/6/7/9/10 (p0 = 32) .478 -> .720; 2/3 (48) .567 -> .94; 4 (64) .656 -> .94; 5/11/12 (20) .411 -> .450; 8 (127) .94 -> .94 (the games send their own parameters, so the table defaults are only a risk indicator). Risk: higher p0 now clamps at .94 sooner (p0 >= 42), and the mapping through the origin is unverified below p0 = 40.
- Stereo width (HLE LR correlation 0.856 / 0.880 vs PCB 0.699 / 0.539; the MAME-derived emulation is 0.755 / 0.943, so both are too correlated). Diagnostic: the driver's ESP program leaks the left delay/reverb input to the right output at -10.5 / -11.1 dB energy (amplitude about 0.3) and not at all for the dry input; the HLE reverb had no input cross-feed (left/right reverb chains use different comb lengths, 1613-1999 vs 1637-2027 samples). Test: feed a fraction x of the opposite channel's reverb send into each comb input, joint re-sweep of the decay against both PCB tails (x = 0, .3, .6, 1.0): at x = .6 and decay .90 the correlation gap shrinks from +0.157 / +0.341 to +0.13 / +0.20 but the tail error rises from 1.4 / 1.7 dB to 1.7 / 2.0 dB (decay .905: 2.4 / 1.4; .895: 1.6 / 3.0); x = .3 and 1.0 are worse on one of the recordings. The tail fit is hurt on both recordings for a partial correlation gain, so no cross-feed was added. The remaining width gap is mostly outside the reverb: the dry OTIS pair 1 renders exactly mono (L = R, correlation 1.000 in the emulation with the other pairs muted), so the PCB's wider image points at voice panning/volume handling in the emulated ES5505 or at pair assignment rather than at the reverb (same in HLE).

## 5. DAC/output stage beyond L/R (low priority)

- Pump summing: MAME's `add_route` 0.5 per pump output pair is not on the board (section 2). The reference engine already reproduced the 0/2 -> L, 1/3 -> R pairing by summing; the board has the single ESP output.
- Spectrum (superseded by section 6): octave-band shares relative to total (0-.5k, .5-1k, 1-2k, 2-4k, 4-6k, 6-8k, 8-10k, 10-12k, 12-13.5k, 13.5-14.7k; recording A): PCB [-1.7, -7.8, -9.9, -15.3, -19.6, -22.3, -26.3, -29.4, -32.9, -35.5]; MAME-derived emulation [-1.7, -7.6, -10.0, -14.6, -19.8, -22.4, -26.5, -29.5, -33.2, -37.3]; HLE [-2.0, -7.9, -8.7, -12.8, -19.4, -23.1, -28.9, -32.7, -38.8, -46.9]. The recording has no rolloff beyond the emulation's up to 13.5 kHz (PCB 1.8 dB above it at 13.5-14.7 kHz): no evidence for an analogue low-pass or de-emphasis, and HLE's 13.8 kHz post filter is not supported (disabling it recovers 0.6/1.6/3.2 dB of HLE's 2.4/3.3/5.9 dB deficit at 8-10/10-12/12-13.5 kHz; the rest is synth interpolation). Left unchanged.
- DAC saturation: both renders of A peak at 0.50 FS; no clipping seen.
- MB87078: modelled per channel pair (SingleStage default); not measurable from one volume setting.

## 6. HLE changes from the clap-dynamics diagnosis (filter index, post filter, native rate)

Target = the 0x187 PCB recording. Band error in dB re PCB, 3..17 s after alignment by content cross-correlation (Welch 4096, high-passed at 20 Hz, mono sum; bands 100-1k, 1-2, 2-4, 4-6, 6-8, 8-10, 10-12, 12-13.5, 13.5-14.7 kHz); PCB levels -26.4 -33.2 -38.4 -42.7 -45.4 -49.3 -52.4 -56.0 -58.5 dBFS:

| render | errors | max |err| 8-14.7 kHz |
|---|---|---|
| MAME-derived emulation | -0.9 -0.8 -0.2 -0.9 -0.9 -0.9 -0.8 -1.1 -2.6 | 2.6 |
| HLE before (48 kHz, 13.8 kHz post low-pass, filter index >> 9) | -1.6 -0.6 0.7 -1.2 -2.0 -3.7 -4.4 -6.9 -12.4 | 12.4 |
| HLE + direct-voice filter index fix + post low-pass removed (48 kHz) | -0.9 -0.5 0.9 -0.5 -0.8 -1.0 -1.2 -2.2 -5.8 | 5.8 |
| **HLE + native 29761 Hz engine (final)** | -0.9 -0.5 0.2 -0.9 -0.9 -0.7 -0.8 -1.1 -2.5 | 2.5 |

All bands are within 1.1 dB of the PCB except 13.5-14.7 kHz (-2.5 dB), where the MAME-derived emulation shows the same deficit (-2.6 dB); it is therefore not specific to the HLE voice stage.

1. Filter index: the driver's direct-voice key-on code (c195a4-c195d0, excerpt in `2026-10-16-kirameki-187-clap-dynamics.md`) computes `(patch[0xe] & 15) * word8 << 7 >> 16`, with word8 the voice's +8 word holding the velocity in both bytes (velocity*257, the same value HLE modulation source 6 uses; the second note-start routine at c192ee fills only the high byte, which explains the 947/948, 691/692 pairs seen in the driver's writes). HLE now uses `direct_filter_index()` (`synth.hpp`) = `patch[0xe]*4 + ((patch[0xe]&15) * velocity*257 >> 9)`, replacing the unit-mismatched `* velocity >> 9`; velocity in HLE is the 7-bit note velocity (bit 7 cleared, `synth.cpp` 1041), the same units as the driver's `$d81a` byte. Pinned by `HleAudio.DirectVoiceFilterIndexMatchesDriver`.
2. The 13.8 kHz 2nd-order post low-pass is removed (`Effects::post_filter` is now `dc_block`, the 20 Hz DC blocker only).
3. `SAMPLE_RATE` is 29761 Hz (ES5505 output grid, = `NATIVE_RATE`, so `RATE_RATIO` is exactly 1) for synth, effects and the PCM engine; the engine outputs at that rate like the reference backend (the frontend opens SDL at `Audio::sample_rate()`, the WAV writers use it), so no resampler is added. Effects: comb/all-pass/delay taps keep their millisecond lengths through `scale_tap`; the per-sample one-pole smoothers (comb damping, the .75 send low-pass) are converted to the same cutoff with `pole_at_engine_rate`; the cross-delay length is now the exact native `p2 * 112` samples. The reverb tail needed a refit because the discretisation of the in-loop damping changed: decay .92 and damping .40 (48 kHz-equivalent) at p0 = 40, p6 = 0x68 (previous .90/.25 gave 14.7/8.0 dB tail error at the new rate; sweep decay .90-.94 x damping .15-.5): tail error 1.8 / 1.1 dB (A/B), LRcorr 0.866 / 0.877 (PCB 0.699 / 0.539, unchanged problem), RMS error -1.8 / -0.4 dB.
4. CPU cost: `sound-extract --sound 0x187 --seconds 24 --tail 30` takes 5.87 s wall against 5.65 s for `--seconds 1`, so 23 s of audio cost about 0.22 s of wall clock in total, about 1% of one core (~100x realtime), with the effects/voice work on the audio worker thread; at 48 kHz the same run took 5.7-6.2 s, no measurable difference (the lower rate renders 38% fewer frames).

## 7. (withdrawn) first "89-instruction disassembly" and 6180-sample tank topology

Superseded by section 8; kept only so the wrong numbers are not rediscovered as facts. Everything in the earlier version of this section was wrong in the same way: the delay RAM offsets 2011..8191 were read as taps of one contiguous 6180-sample line (tap = offset - 2011), and the instruction listing mixed operand fields (several "Read Delay N" lines were dumped GPR values of instructions whose RAM data is never consumed, and the "G89 = tap sum" annotations were guesses). The ES5510 delay address is `(dbase + offset) >> 8` with `dbase` decreasing by one address per frame, so a read at offset R sees what the write at offset W stored `(R - W) / 256` frames earlier, inside one 64K-word ring; there are no separate segments and no 6180-sample loop. The loop of the program has a 1105-frame return and nested all-pass stages of 168/276/755/549/1369/1012/696/242 frames. The "verification" tables of that section (clap window, whole-song metrics) measured a topology the program does not have and are not valid evidence.

## 7. Reverb drive follow-up (see 2026-10-16-kirameki-187-clap-dynamics.md section 12)

(Superseded by section 8: `r_norm`, `K_DECAY_PER_P0` and the comb/tank constants no longer exist.) `effects.hpp` then had the reverb-send normalisation `1/(1+cross)` back and the tank decay refitted to `K_DECAY_PER_P0 = 0.63/40`. Against the PCB: A (0x187) level error -0.92 dB, tail mean |err| 1.74 dB (bias +1.46), L-R correlation 0.805 at the song end (PCB 0.699); B (sound 1) level error +0.22 dB (was +1.47), tail mean |err| 2.38 dB (bias -2.28), L-R correlation 0.626 (PCB 0.539). Pair 2 keeps the parametrised cross delay (p1 feedback, p2 delay) with direct gain table(127 - p8) and no send low-pass.

## 8. Decoded ES5510 program 5 and the HLE rebuilt from it (supersedes the old section 7)

Commands (all from the repository root; `build-kirameki` as elsewhere):

```
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 8 \
    --audio-backend reference --sound-driver native --sound-trace OUT.trace --wav OUT.wav
python tools/compare/esp_graph.py OUT.trace disasm          # loaded program
python tools/compare/esp_graph.py OUT.trace graph           # exact signal graph (about a minute)
python tools/compare/esp_graph.py OUT.trace verify --input 4 --frames 8000   # HLE graph vs emulator impulse
uv run --group analysis python -m tools.compare.reverb --pcb PCB --render hle=HLE.wav --render ref=REF.wav
```

`tools/compare/esp_graph.py` replays the trace's ES5510 host writes into a pure-Python integer port of `runtime/third_party/audio/es5510.cpp` (same pipeline order; bit-identical to the C++ over 3000 frames of noise-floor output in the check made during development), runs it symbolically (every register also carries a linear expression over the inputs, the delay RAM writes and the previous frame's registers; delay reads are attributed to their writer from the real RAM addresses) and prints the resulting netlist. The state used was the program the driver loads for Kirameki preset 5 with parameters 40 0 20 26 37 0 104 64 32 39 (p0..p9; the trace shows the driver's GPR writes: G110 = 40<<16, G1 = .375, G133 = 983, G107 = 1482).

### 8.1 Findings that change the earlier picture

1. **Delay RAM is one 64K-word ring.** Address = `((dbase + offset) mod (dlength + 256)) >> 8` (memshift 8, dlength 0xffffff), `dbase -= 256` per frame. Instruction N's offset is GPR N (the same register is also a coefficient/variable, so offsets such as 8192 or 32522 are often simply a coefficient and the RAM word read is never used). A delay is `(read offset - write offset) / 256` frames: all reads the program consumes are 4..1369 frames long, plus LFO-modulated ones.
2. **Reads of "dump" cycles still load DIL.** `Read Delay and Dump FIFO` discards a DOL entry but the read data is available; MAC results sent to DOL with such a cycle (`DOL = ...` entries) only feed the accumulator.
3. **The RAM also holds constants.** The program writes constants (40 and -1, 1) into RAM each frame and reads them back as DIL (LFO depth 40 frames, an AND mask for the fractional part, +1 frame for the interpolation pair). Zeroing the RAM of a running program removes them for one ring period only; any impulse test must run a few frames first.
4. **p2 is the chorus LFO rate, not an echo length.** In program 5 G133 and G107 (written from p2) are the rate constants of the two chorus LFOs; the 274.8 ms cross delay of the earlier HLE belongs to another program (the one measured on preset 11) and does not exist here.

### 8.2 The graph (frame t, all values float, gains = GPR / 2^23; z-d = d frames ago; primed = register latched at the end of frame t-1)

Inputs: a0 = ser0 (dry, OTIS pair 1), a1 = ser1 (pair 2), a2 = ser2 (pair 3), each L/R. Output ser3 = DAC.

```
OUT_L = a0L + G125*G115' + G123*a2L + G89'          (G127 = 0.99997 omitted)        G125 = table(127-p8), G123 = table(127-p9)
chorus:  W29 = a1L + G134*G130';  W34 = a1R + G134*G129'                             G134 = p1/128
         G128' = tap(W29, 70  + 40*xA)  G129' = tap(W29, 303 + 40*xB)                 tap(d) = linear interpolation between z-floor(d) and z-floor(d)-1
         G130' = tap(W34, 261 + 40*yA)  G131' = tap(W34, 428 + 40*yB)
         G115' = sin(th)*0.7071*(G128 + G129) + cos(th)*a1L                           (sin, cos) = (G118, G119); th ~ 48 deg at p6 = 0x68
         G116' likewise from G130, G131 and a1R
reverb input:  u = 0.7018*(G124*(G115' + G116') + G122*(a2L + a2R)) + 0.0075*u[t-1]     G124 = table(p8), G122 = table(p9)
loop return:   ret = W48[z-1105];  G70' = 0.625*ret + 0.375*G70;  G71' = 0.125*G70 + 0.875*G71        (0.375 = G1 = (p6>>4)/16)
               v = u + G110*(G2*ret + G1*G70') + 0.00703*G70' + 0.04922*G71'                          G110 = p0/128, G2 = 1 - G1
diffusers:     W64 = v + 0.4*W64[z-168];  W69 = -0.4*v + 0.84*W64[z-168] + 0.4*W69[z-276];  W68 = 0.16*v - 0.336*W64[z-168] + 0.84*W69[z-276]
block B:       P = W68[z-755], Q = W87[z-549], R = W88[z-1369]
               W85 = -0.5P + 0.75Q;  W87 = -0.25P - 0.125Q + 0.9375R;  W88 = 0.25R + P + 0.5Q
block A:       A = W50[z-1012], B = W85[z-242], C = W53[z-696], M = G91'
               W48 = 0.75A - 0.5B;  W50 = -0.125A + 0.9375C - 0.25B;  W53 = -0.125A - 0.0625C - 0.25B + 0.9375M;  W54 = 0.5A + 0.25C + B + 0.25M
               G91' = tap(W54, 921 + 8*xC)
wet taps:      G89' = 0.1333*W48[z-384]  + 0.6667*W68[z-4]   + 0.4667*W85[z-26]
               G90' = 0.1333*W48[z-1105] + 0.6667*W68[z-242] + 0.6667*W85[z-180]
LFOs:          A: (xA, yA) eps G133 (0.555 Hz at p2 = 20); B: (xB, yB) eps G107 (0.84 Hz); C: eps 4864/2^23 (2.75 Hz); x' = x + eps*y, y' = y - eps*x
```

The loop (decay element) is `W48 -> [1105] -> v -> 168/276 diffusers -> W68 -> [755/549/1369 block B] -> W85 -> [242] -> block A -> W48`; the loop gain at DC is `G110 + 0.0562` (0.3687 for p0 = 40), the second damping pole and the 0.0070/0.0492 terms are independent of p0.

Compared with the earlier HLE: the "taps L 450/3148/5459, R 688/3302/6179" weights 0.6667/0.4667/0.1333 and 0.6667/0.6667/0.1333 are right but the delays were not (real: 4/26/384 and 242/180/1105 inside the loop nodes W68/W85/W48), the loop is not 6180 but ~4000 frames of nested all-passes with a 0.37 DC loop gain, the input is the mono sum, and `r_norm = 1/(1+cross)` has no counterpart: the reverb send reaches the DAC directly with `table(127-p9)` and enters the tank with `0.7018*table(p9)` (mono sum, no cross-feed multiplier).

### 8.3 Reproduction of the emulator

`verify --input 4` (impulse on ser2l, 8000 frames, LFO registers taken from the port): max error 8.6e-5 = -76.7 dB re peak, error rms 2.7e-5..6.1e-5 in all time windows; the same check with the linear netlist itself (the `graph` output evaluated per frame, 12000 frames, three inputs) gives -73 / -76 dB (ser1l/ser2l) and -92 dB (ser0). The residue is the emulator's 16-bit truncation (DC -7.5/-8.7 LSB, noise rms 0.65 LSB). `--input 2` (chorus input) differs by at most 0.6% of a tap peak on four frames: the hardware quantises the interpolation fraction to 1/256 frame, the model does not.

### 8.4 Emulator verdict (point 3 of the brief)

The "plateau instead of decay" is **not** a decay failure of the ES5510 emulation. (a) The decoded loop gain is 0.37 and the impulse responses of the real emulator (ser1l and ser2l at 0.61 FS, 90000 frames) fall to a flat 0.65 LSB rms floor within 1.25 s (energy decay re total: -17/-24/-33/-37/-39 dB at 0.05/0.2/0.5/0.75/1.5 s, the rest is the early response); (b) the reference render after the song end stays at a constant -56.7 dBFS because it is the DC offset the emulated ESP carries (mean -50.9/-44.1 LSB of the output, std 3 LSB over 23-26 s), produced by `round_24_to_16` flooring (`>> 8`) of every delay RAM write and amplified by the output gain; the PCB output is AC coupled, so tail comparisons must high-pass the reference (tools/compare/reverb.py does; the earlier tables here did too). Nothing was changed in es5510.cpp. One real quirk of the emulation was noticed and is **not** fixed because the hardware behaviour is unknown: the wrap is `dbase = dlength` (0xffffff) instead of `dbase + 2^24 - 256` and the delay modulus is `dlength + 256`, so once per 65536 frames and for addresses that wrap the delays are one address off; it is inaudible but means the reference is not bit-exact to a ring of 65536 words.

### 8.5 What is derived and what is fitted

Derived (program graph or the driver's GPR writes): every delay length, every dyadic coefficient (0.4, 0.84, 0.16, 0.336, 0.5, 0.75, ..., 0.9375), the tap weights, 0.7018 = G59*G4, 0.0075, the loop filter constants 0.00703/0.04922, LFO depths (40 and 8 frames), LFO centres, the third LFO rate, p0 -> G110 = p0/128, p1 -> G134 = p1/128, p6 high nibble -> G1 = n/16 (p6 = 0x68 -> .375 and the boot preset -> .25), p8/p9 -> the four table() gains (driver trace: p8 = 32 -> G124 = .3855, p9 = 39 -> G122 = .4639).
Fitted to two observations only: p6 low nibble -> (G118, G119) = (sin, cos)(n*6 deg) (observed .7453/.6667 at n = 8 and 0/1 at n = 0; the formula gives .7431/.6691) and p2 -> LFO rates (linear through p2 = 20 -> 983/1482 and 84 -> 1327/2003 for G133/G107). No PCB-fitted constant remains: the removed `K_DECAY_PER_P0`, `K_DAMPING_SCALE`, `r_norm`, 0.2/0.25 delay-to-reverb sends and 0.70 delay wet factor are gone. Limitation (resolved in section 9): only program 5 was decoded at this point; section 9 decodes the other programs the three profiled games reach and corrects the parameter numbering and several mappings of this section (the p6 low-nibble angle is `table(8n+4)`, the p2 LFO rate is `196.608*(p2-15)/2^23`).

### 8.6 PCB metrics (tools/compare/reverb.py; HLE rendered with `--seconds 30`, tail rows from a `--seconds 21` render because the HLE sequencer restarts the song at ~21.1 s, being fixed elsewhere)

0x187 (flac), before = HLE as of the start of this task, ref = reference backend:

| metric | PCB | before | after | ref |
|---|---|---|---|---|
| rms_db | -20.69 | -20.99 (-0.30) | -21.62 (-0.93) | -21.61 (-0.92) |
| C50 300-2k / 2-6k / 6-14k (diff dB) | -6.40 / -5.76 / -3.19 | +0.30 / -0.40 / -0.57 | +0.23 / +0.20 / +0.20 | +0.42 / -0.71 / +0.38 |
| floor p10-p90 (diff) | -8.05 | +1.08 | -0.36 | -0.18 |
| L/R correlation | 0.7129 | 0.8258 | 0.7210 | 0.7543 |
| side/mid dB | -7.74 | -10.17 | -7.85 | -8.50 |
| tail_mae / tail_bias (dB, +0.1..+0.75 s) | - | n/a (EOF) | 2.10 / -0.12 | 7.80 / +0.95 (DC plateau) |

after tail_db diffs at +0.1/.2/.3/.5/.75 s: +2.80 +1.11 +1.06 -1.19 -4.36 (PCB -27.8 -30.5 -32.3 -37.9 -41.9). The late-tail slope difference is voice release (the HLE synth, not the effects): the reference, which has the same ESP, is 5-8 dB below the PCB at +0.1..+0.3 s. RMS -0.93 dB equals the reference's -0.92 dB, i.e. it is outside the effects stage (the ESP graph is exact), so no gain was fitted to remove it.

Sound code 1 (wav; the script flags the alignment low-confidence: envelope r 0.42, HLE render shorter than the PCB body): rms diff +0.03 (before +2.05), C50 -2.38/-1.10/-3.72 (before +0.16/+0.70/-0.23), floor -0.10, L/R correlation 0.806 (PCB 0.562, before 0.672, ref 0.918), side/mid -9.70 (PCB -5.74, before -7.08), tail not comparable (HLE `end level` -48.9 vs PCB -26.3). The acceptance targets for code 1 (L/R closer to PCB, tail MAE <= 2 dB) are **not met**: the reference with the exact same ESP is at 0.918 and the dry/send panning of the voices, not the effects stage, decides this recording; these numbers are low-confidence and are reported only for completeness.

## 9. All programs the three profiled games reach, and the parameter mapping read from the driver

Brief: landmakrj (preset 11) and commandw (preset 1) load other ES5510 programs than Kirameki's preset 5, so the section 8 graph must not serve them. Findings, evidence and the HLE rebuilt per program:

### 9.1 Which program a preset loads

The sound ROM (identical in all three games: `e44-20.51`/`e44-21.52` have the same CRC/SHA in the three manifests) has a program pointer table at 0xc1c3a2 (11 entries) and a preset table at 0xc1c2d2 (13 rows of 16 bytes). **Row byte 5 indexes the pointer table; row bytes 6..15 are p0..p9 in forward order** (the sound command `0x21 idx val` sets p_idx; the earlier HLE `select()` read the row in reverse, which only went unnoticed because every Kirameki song sends p0..p9 afterwards). Evidence: Kirameki's preset 5 loads the program whose first 60 instruction words equal the ROM stream at pointer 5; commandw's preset 1 (row byte 5 = 0) and Kirameki's boot preset load pointer 0; landmakrj's preset 11 (row byte 5 = 6) loads pointer 6. Pointer table: `0:c1c836 1:c1c4e2 2:c1c4e2 3:c1cb19 4:c1ce1b 5:c1d171 6:c1d4d4 7:c1d823 8:c1db8a 9:c1de02 10:c1e10b` (entries 1 and 2 are the same program).

Reachable programs: a scan of all Kirameki sound codes 0x000-0x3ff (reference selects seen in the packets: preset 0 x15, 1 x2, 2 x4, 4 x3, 5 x80) shows Kirameki uses presets 0, 1, 2, 4, 5; Land Maker uses 11; Command War uses 1. They map to ROM programs 2, 0, 1, 4, 5, 6, 0. Decoded here: **0** (Command War, Kirameki preset 1), **1/2** (Kirameki presets 0 and 2), **4** (Kirameki preset 4), **5** (Kirameki preset 5), **6** (Land Maker preset 11). Programs 3 and 7-10 are not reachable by the three games and make `Effects::select()` throw `std::runtime_error`.

Capture (traces of the reference backend; Land Maker/Command War have no `--sound` profile, so `gameplay` with `--sound-trace`):

```
./build-landmakrj/f3rt-tool gameplay --rom-dir roms/landmakrj --set landmakrj --frames 12000 --sound-driver native --sound-trace L.trace --wav ref_L.wav
./build-commandw/f3rt-tool  gameplay --rom-dir roms/commandw  --set commandw  --frames 12000 --sound-driver native --sound-trace C.trace --wav ref_C.wav
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x19b --seconds 4 --audio-backend reference --sound-driver native --sound-trace K19b.trace   # preset 0
                                                       ... --sound 240 (preset 2)   ... --sound 303 (preset 4)   ... --sound 0x187 (preset 5)
python tools/compare/esp_graph.py TRACE programs   # which programs were loaded and whether they are decoded
python tools/compare/esp_graph.py TRACE graph / disasm / verify
```

### 9.2 Programs

All four non-mute programs share the tank of section 8 (1105 return, 168/276 diffuser pair, nested all-pass blocks 755/549/1369 and 1012/696/242, modulated tap 921, wet taps 384/4/26 and 1105/242/180) with program-specific constants; they differ in the serio1 path.

| program | loaded by | serio1 path | tank constants (GPR derived) |
|---|---|---|---|
| 0 Reverb (0xc1c836) | Command War preset 1, Kirameki preset 1 | direct, same frame (no latency) and into the reverb | k = 0.5, leak G3 = 0.0703 (p6 = 94), c1 = G108/8, c2 = 0.875 G108, g = G8 = p7/128, taps .125/.625/.4375 (L), .125/.625/.625 (R); the reverb input is the mono sum of `table(p8)*serio1 + table(p9)*serio2` |
| 5 Chorus (0xc1d171) | Kirameki preset 5 | 4-tap chorus (section 8) | k = 0.7071, leak .0075, G108 = .05625, g = .4, taps .1333/.6667/.4667 and .1333/.6667/.6667 |
| 6 Echo (0xc1d4d4) | Land Maker preset 11 | ping-pong echo p2*112 frames, feedback p1/128, enhancer on the left input; the block A tap reads the left echo line at 256*p1 frames | as 5, decay G110 = .25, G1 = .3125 |
| 1/2 Hall (0xc1c4e2) | Kirameki presets 0, 2 | none (per side input) | two chains, see below |
| 4 Mute (0xc1ce1b) | Kirameki preset 4 | - | one END; the reference renders sound codes using it (303, 765, 834) as silence |

**Echo (program 6).** `W33 = a1L + g113 + G134*G111'`, `W35 = a1R + G134*G105'` (the two echo lines, 8176 = 73*112 frames for p2 = 73); `G105' = tap(W33, 8175.996)`, `G111' = W35[z-8176]`; `G97 = sin*.7071*G105 + cos*(a1L + g113)`, `G98 = sin*.7071*G111 + cos*a1R`; the left input enhancer is `y = sat(4*sat(4*a1L))`, `g112 = sat(2y - y^3/2)` (the ASL2/ASL2/square/cube in instructions 11-20), `g113 = .75*g113 + (G143/4)*g112`, with G143 = (n^2 + 3)/256 for the p6 high nibble n (a small-signal boost of about +5 dB below -30 dBFS, saturating at .027). Block A's modulated tap `G78` reads `W33` at `G0/256 + 8*xC` frames, not the all-pass line: the driver's scratch writes leave G0 = p1<<16 | 0x7e (0x3a007e for p1 = 58 = 14848.49 frames); bursts of other G0 values (0x1e0000, 0xd0000, ...) every ~3.4 ms in the trace are not modelled. The graph reproduces the reference impulse (`verify`, echo 8176, G0 14848.49): -73.4 dB re peak (ser2l) and -73.5 dB (ser1l).

**Hall (programs 1/2).** Per chain (X = right input, Y = left input): diffuser pair (X 88/141, Y 102/168 frames; g = G8 = p7/128, g2 = G17 = .796875 g): `W30 = v + g*W30[z-d1]`, `o1 = -g*v + (1-g^2)*W30[z-d1]`, `W35 = o1 + g2*W35[z-d2]`, output `(1-g2^2)*W35[z-d2] - g2*o1`; nested all-pass block B (a = G5 = (p0+7)/128; X delays 628/232/874, Y 449/335/836): `out = -a*P + (1-a^2)*Q`, `Q' = -.25 P - a/4 Q + .9375 R`, `R' = .25 R + P + a Q`; nested all-pass block A (X 650/369/214, Y 617/424/145) with the LFO-modulated tap: `o2 = (1-a^2) A - a B`, `A' = -a/4 A + .9375 C - .25 B`, `C' = -a/4 A - .0625 C - .25 B + .9375 M`, `o4 = a A + .25 C + B + .25 M` (M = tap(o4_X, 611 + 8 xC), tap(o4_Y, 475 + 8 xC)). Chain input `v = .5*u + G110*LP1 + G108*LP2` where X's low-passes (pole G1 = .5 and .875) filter Y's block A output at 675 frames and Y's filter X's at 7 frames (the figure of eight), `u_side = leak*u + (1-leak)*(table(p8)*a1 + table(p9)*a2)` per side, leak .0078. Wet taps: `L = .125 W13[3] + .4375 W23[163] + .9375 W34[101] - .4375 W69[131]`, `R = -.4375 W23[74] + .125 W44[225] + .4375 W69[10] + .5 W80[1]`. Output `a0 + table(127-p8)*a1 + table(127-p9)*a2 + wet`. `verify`: -74.7 dB (preset 0) and -73.7 dB (preset 2).

**Reverb (program 0).** The graph of section 8 with the constants above; the loop-filter terms are `c1 = G108/8`, `c2 = 0.875*G108` for every program (section 8 found .00703/.04922 as numbers: they are G108 = .05625 of program 5). `verify`: -71.2 dB.

### 9.3 Parameter mapping (read, not guessed)

Method: the CRC checks were skipped by a temporary edit (reverted), a ROM copy's preset row was copied over preset 5 (which Kirameki code 413 selects without further commands), one row byte was changed by +1 and +16, the reference was run, and the changed GPRs were listed (script: `2026-10-16-esp-program-param-sweep.py.txt`). Index j below = row byte 6 + j.

| j | Reverb (0) | Chorus (5) | Hall (1/2) | Echo (6, as seen in Land Maker) |
|---|---|---|---|---|
| 0 | G110 = p0/128 | G110 = p0/128 | G110 = p0/128 and a = G5 = (p0+7)/128 | (row path: G110; Land Maker's driver never writes it: .25) |
| 1 | G108 = p1/128 | G134 = p1/128 | G108 = p1/128 | G134 = p1/128, G0 = 256 p1 + .49 |
| 2 | LFO C rate | LFO A/B rates | LFO C rate | G104 (echo = 112 p2 frames) |
| 3 | LFO C depth = p3/2 frames | - | depth p3/2 frames | - |
| 6 | high nibble: G1 = n/16; low nibble: G3 (leak, not decoded) | G1 = n/16 (high); (G118, G119) = (table(8m+4), table(127-(8m+4))) for the low nibble m | G1 = n/16 (high) | G143 = (n^2+3)/256 (high); (G118, G119) as Chorus (low) |
| 7 | g = G8 = p7/128, G14 = 1-g^2 | - | G8 = p7/128 (G17 = .796875 g, G16 = 1-g^2, G27 = 1-G17^2) | - |
| 8 | G124/G125 = table(p8)/table(127-p8) | same | same | same |
| 9 | G122/G123 = table(p9)/table(127-p9) | same | same | same |

LFO rate: `eps = 196.608*(p2-15)/2^23` rad/frame (raw 983 at p2 = 20, 4915 at 40, 12780 at 80; zero below 15; program 5's G107 = 296.4*(p2-15), G133 = 196.608*(p2-15)); programs 5 and 6 keep LFO C at raw 4864. p3, p4, p5, p7 (Chorus/Echo) and p4, p5 never changed a GPR. This replaces the two-point fits of section 8: the p6 low nibble angle is exactly `table(8n+4)` (n = 8: .74533/.66666, n = 9: .80753/.58975), and the p2 mapping is linear in (p2-15), not the earlier line.

Command path vs row path: Command War follows the row mapping (G108 = 9/128 = p1, g = 52/128 = p7). Land Maker's driver (preset 11, row bytes 0..1 = 0) does not apply the row mapping of G110/G108/G1 (they stay at the program defaults .25/.05625/.3125) but does apply p1, p2, p6, p8, p9 from its commands; the HLE follows the observed behaviour. For the Hall presets the commands that follow the select in the Kirameki packets (e.g. 0x19c) arrive while the previous program is still loaded and are lost (the reference GPRs after those codes equal the row values), so only the row sets those programs' parameters in practice; the HLE applies later parameter commands if they do arrive.

### 9.4 Verification

`esp_graph.py verify` against the reference impulse (20000 on ser2l, ser1l; 6000 frames; error vs the emulator's 16-bit truncation floor): program 0 -71.2 dB, program 1 -74.7 (preset 0) and -73.7 (preset 2), program 5 -75.5 (ser2l; 8000 frames -76.7), program 6 -73.4 / -73.5, all re peak. The C++ `Effects` against the Python models with identical LFO sequences and parameters (40000 frames, DC blocker applied): program 5 -92 dB, program 0 -85 dB, program 1 -96 dB, program 6 -152 dB on serio2 and -67 dB on serio1 (float32 precision of the interpolated taps at 14848 +-8 frames). EDC of the C++ against the reference impulse (real volume table): early decay within 0.7 dB to 0.2 s for all four programs; the reference EDC flattens at about -40 dB (its 0.65 LSB truncation noise) so later values are not comparable.

Sanity vs the reference renders (no PCB exists for these games; 5..190 s of the 12000-frame gameplay, mean of both channels): landmakrj reference rms -31.11 dB / L-R correlation .928, HLE -31.04 / .925; commandw reference -30.43 / .818, HLE -30.07 / .848 (reverb.py cannot analyse the commandw render: it reports a padlen error). landmakrj reverb.py with the reference as target (low-confidence alignment, window 5-60 s): C50 300-2k/2-6k/6-14k -0.26/-1.24/-2.10 dB (the previous program-5 graph: -0.40/-1.02/-1.70), L/R correlation .881 vs .892, side/mid -11.6 vs -12.2 dB. Kirameki 0x187 vs the PCB is unchanged by this work: C50 +.23/+.20/+.20 dB, floor -.37, L/R .7205 (PCB .7129), side/mid -7.84 (-7.74), tail MAE 2.67 / bias -1.02 over +0.1..+1.0 s (the earlier 2.10/-0.12 used the five points up to +0.75 s).

HLE behaviour changes: a program that is not decoded throws from `select()` (a game that selects one stops its audio worker instead of sounding like another program); before the first preset select the HLE plays the dry input only; preset 4 is silent as on the hardware.
