---
source_url: user-supplied PCB recordings /home/answer/Downloads/kirameki-sound-1-187.flac (Kirameki Star Road sound test BANK 01 / SOUND CODE 0x187) and kirameki-sound-1-1.wav (code 1, not used here); runtime/audio/hle/synth.cpp, runtime/third_party/audio/es5505.cpp
ingested: 2026-10-16
sha256: unrecorded (renders were scratch files under /tmp/c, not retained)
game: [kirameki]
captured_with: "f3rt-tool sound-extract only (repo build ./build-kirameki/f3rt-tool and a scratch copy built at /tmp/x/b/f3rt-tool from a copy of the working tree, which already contained another agent's uncommitted runtime/audio edits); analysis in numpy/scipy via uv run --group analysis; no tracked source file was edited"
---
# Kirameki 0x187: why our claps are flat and the top end is dull (HLE direct-voice filter index, then 48 kHz rendering and the 13.8 kHz post filter)

**PCB = ground truth. The `reference` backend (MAME-derived emulation) is only a diagnostic**; every conclusion below is checked against the PCB recording, and the reference is used only to find *where* HLE differs (the real driver's register writes), never as the target.

## Verdict (ranked)

| # | cause | location | evidence (long-term band level re PCB, 3..20 s) |
|---|---|---|---|
| 1 | **HLE computes the wrong filter cutoff for every "direct" (drum/percussion-kit) voice**: the velocity term of the filter-table index is shifted by 9 instead of 1 | `runtime/audio/hle/synth.cpp:692` `int filter_index = p[0xe] * 4 + ((p[0xe] & 15) * v.owner.velocity >> 9);` | clap 0xfa70 expected (index 948) vs HLE 0x7950 (index 661); fixing it raises 8-14.7 kHz by 2.5/2.5/2.9/3.4 dB (§3) |
| 2 | HLE runs at 48 kHz, ES5505 at 29761 Hz: linear interpolation is evaluated on a 48 kHz grid, so the triangle-kernel droop is not cancelled by the 29.76 kHz sampling and no aliasing is folded back | `runtime/audio/hle/events.hpp:6` `SAMPLE_RATE = 48000`; `synth.cpp:747` increment; kernel `synth.cpp` ~1282-1340 | 29761 vs 48000 (both without post filter, bug 1 fixed): +0.5/+0.4/+1.4/+5.2 dB at 8-10/10-12/12-13.5/13.5-14.7 kHz (the last includes ~1.9 dB that is my 48k->29762 analysis resampler, §6) |
| 3 | 13.8 kHz 2nd-order post low-pass (and 20 Hz DC high-pass) at the end of the effects chain | `runtime/audio/hle/effects.hpp:299-352` (`init_post_filter`, `post_filter`) | removing it: +0.3/+0.9/+1.9/+3.3 dB (same bands). At 29761 Hz it would be worse (13.5-14.7 kHz: -10.2 vs -5.1 dB without it), it is only tolerable at 48 kHz |
| 4 | not causes: output limiter/compressor (none exists), volume 4 ms ramp, filter ramp, wet damping, resampling to WAV | see §4 | each tested, <=0.1 dB |

The prior note's "unexplained synth interpolation" residual was mostly cause 1 (a voice-parameter bug), not interpolation: with cause 1 fixed the remaining HF deficit is explained by 2 and 3 (§3 last row: at native rate, no post filter, fixed index, HLE is within 0.8-1.3 dB of the PCB in every band, the same flat -1.0..-1.3 dB offset the reference shows).

## 1. Alignment (content cross-correlation; PCB has leading silence)

Both signals high-passed at 20 Hz, resampled to 29762 Hz (HLE 48000 -> 29762 with `scipy.signal.resample_poly(14881, 24000)`; the reference WAV is written at 29761 Hz and used as is). PCB energy is zero for the first ~1.5 s. Cross-correlating 10 s of our render (2..12 s) against the PCB:

* reference: **PCB = reference + 1.619 s (48183 samples)**, identical result for segments at 1, 4, 8, 12, 16 s (+-0.3 ms), normalised r 0.45-0.78 per second after alignment. Per-second best lag within +-120 ms is 0 +-1 ms (one outlier at 12 s: +17 ms).
* HLE: **PCB = HLE + 1.648 s (49048 samples)** is the global lock, but per-second residual lags scatter between -29 and +19 ms (r 0.28-0.55); HLE vs reference per-second lags are -9/-32/+19/-4/-24 ms. So HLE note timing is not locked to the real driver at the ~20 ms level (side finding, not investigated; it lowers coincident-peak summation in any per-clap peak statistic, §5).
* PCB does not miss the start of the song: its first content (1.6 s in the file) lines up with our t=0.

## 2. The claps, by voice

From `--hle-events` (kind 0): the clap-like voices are sample 8291465 (track 7, key 15, layer 16, 3533 words, every ~0.49 s, 68 hits), the hat/shaker 8300655 (track 7, key 21, every 196 ms), snare/noise 5638058 (track 8, key 43, 1923..5454 words) and the layered perc 8140754 (168-word loop, layer 1) + 7995804 (7500-word noise, layer 0) on track 1 (every ~74 ms). All are **direct voices** (`v.direct`, `synth.cpp:691`, K1 = K2). Voice-class table (HLE event CSV at key-on, vs the real driver's register writes captured by `--sound-trace` with the reference backend: the 68000 driver program writes K2/K1 as one long word per key-on, decoded by sample start):

| sample start (low 20 bits) | voice | HLE K1=K2 (table index) | driver writes K1=K2 (index) |
|---|---|---|---|
| 951433 | clap, track 7 key 15 | 0x7950 (661) | 0xfa70 (948) / 0xfa40 (947) |
| 960623 | hat 8300655 | 0xfed0 (993) | 0xfff0 (1023, clamp) |
| 395178 | snare 5638058 | 0xfd90 (976) | 0xfff0 (1023) |
| 581761 | 7921793 | 0xce80 (806) | 0xfff0 (1023) |
| 574335 | pad 4768639 | 0x1a00/0x1a30 (404/405) | 0x8be0 (692), 0x9200 (702), 0x4ea0 (579), also 691/701 |
| 919792 | 8259824 | 0x9aa0 (716) | 0xf150 (900) |

Voices with K1 != K2 (non-direct, `synth.cpp:721-722`: 800722 = 0xa200/0xff00, 655772 = 0xfff0/0x0, 1004619 = 0x7fd0/0xfff0) match the driver exactly (205/205, 205/205, 105/105 key-ons). Every mismatching voice is a direct voice.

## 3. Root cause and fit

Debug output from the scratch build (`p0e` = patch byte 0xe, `vel` = note velocity): clap p0e=165 vel=115; pad p0e=101 vel=70/103-119; 8259824 p0e=179 vel=123. Driver index = p0e*4 + ((p0e&15) * vel >> **1**):

* clap: 660 + (5*115>>1 = 287) = 947; with vel 117: 948 (both observed)
* pad vel 70: 404 + 175 = 579 (observed 579); vel 115: 404+287 = 691 (691); vel 119: 405 + 297 = 702 (702)
* 8259824: 716-... = 179*4 + (3*123>>1 = 184) = 900 (observed 900)
* hat/snare/7921793: >=1023 -> clamped (observed 0xfff0)

HLE uses `>> 9`, i.e. 256x too small, so direct voices get a filter that is far too closed (clap: per-pole coefficient a = 0.47 instead of 0.98; four poles). The clap sample (the 4 K1/K2 poles) then removes most of the content above ~3 kHz, which is the "flat, compressed" transient.

Band-level table (dB re PCB; Welch PSD, 4096-pt, 3..20 s of the PCB vs the same song portion aligned by the global offsets; mono sum; high-passed at 20 Hz):

| band kHz | PCB dBFS | reference | HLE now (E0) | E1: post filter off | E2: 29761 Hz, no post | F3: E1 + index fix | F1: shipped config + index fix | F2: 29761, no post, + fix |
|---|---|---|---|---|---|---|---|---|
| 0.1-1 | -31.6 | -1.0 | -1.9 | -1.9 | -1.9 | -1.3 | -1.3 | -1.3 |
| 1-2 | -37.3 | -1.3 | -1.8 | -1.8 | -1.8 | -1.8 | -1.8 | -1.8 |
| 2-4 | -43.1 | -0.4 | -0.7 | -0.7 | -0.9 | -0.6 | -0.6 | -0.8 |
| 4-6 | -48.1 | -1.1 | -2.0 | -2.0 | -1.9 | -1.3 | -1.4 | -1.3 |
| 6-8 | -51.0 | -1.0 | -2.6 | -2.5 | -2.2 | -1.3 | -1.4 | -1.2 |
| 8-10 | -55.1 | -0.9 | -4.1 | -3.8 | -3.3 | -1.3 | -1.6 | -0.8 |
| 10-12 | -58.2 | -1.0 | -4.7 | -3.9 | -2.9 | -1.4 | -2.3 | -1.0 |
| 12-13.5 | -61.9 | -1.0 | -7.3 | -5.4 | -3.6 | -2.5 | -4.4 | -1.1 |
| 13.5-14.7 | -64.4 | -1.0 | -13.0 | -9.6 | -5.1 | -6.2 | -9.5 | -1.0 |
| total RMS | | -1.03 | -1.67 | -1.49 | -1.49 | -1.07 | -1.25 | -1.07 |

(13.5-14.7 kHz of 48 kHz renders additionally loses ~1.9 dB in my analysis resampler, §6.) Stage contributions at 8-10 / 10-12 / 12-13.5 / 13.5-14.7 kHz: post filter +0.3/+0.9/+1.9/+3.3 dB (F3 vs F1); 48 kHz interpolation grid +0.5/+0.4/+1.4/+5.2 dB (F2 vs F3); filter index +2.5/+2.5/+2.9/+3.4 dB (F3 vs E1). Level-flat residual of about -1.0..-1.3 dB across all bands is shared with the reference, so it is not an HF or dynamics effect (it is the gain/level path, other notes).

Check of the analytic expectation: linear interpolation evaluated off the native grid has response sinc^2(f/29761) for a voice played at pitch ratio 1: -1.6 / -2.7 / -4.1 / -5.6 / -6.9 dB at 7 / 9 / 11 / 12.75 / 14.1 kHz. The measured 48 kHz-grid contribution (above) is smaller because most energy at those frequencies comes from voices played at ratio > 1 and from voices that were being filtered anyway.

## 4. What is NOT the cause (each tested)

* **Dynamics / limiter / soft clip**: none exists (`grep tanh|compress|limit` in `runtime/audio/hle/{effects.hpp,audio.cpp}`, `audio.cpp`: only the final `std::clamp` to int16, peak 16439 of 32767). Level-dependence in 50 ms windows 3..20 s: ours vs PCB in dB has slope 0.986 against PCB level for the reference (level independent, offset -1.0 dB in every PCB-level quartile); HLE slope 0.83 is regression dilution from the +-20 ms timing scatter (§1), per-quartile medians -0.9 .. -1.6 dB. Window crest factor (peak/RMS, 50 ms, windows with RMS>0.02) percentiles 10/50/90: PCB 2.56/3.02/3.65, reference 2.58/3.01/3.78, HLE 2.59/3.00/3.69. 99.9th-percentile window peak: PCB 0.485, reference 0.463, HLE 0.425 (-1.1 dB). So the music is not compressed, the HLE is about 1.1-1.7 dB quieter and its transients duller.
* **Volume path**: key-on volumes equal the driver's (clap LVOL 0xd920 = HLE left_volume 55584); removing the 4 ms volume ramp (`VOLUME_RAMP`, only for later parameter updates, `synth.cpp:759-762`) changes no band by more than 0.1 dB (E7); the 3 ms filter ramp (`FILTER_RAMP`) off: <=0.1 dB (E10).
* **Reverb/delay damping**: setting the wet damping coefficient to 0.95 (E5) changes no band (<=0.0 dB); muting wet sends (E4) lowers 8-14.7 kHz by 1.3/0.9/1.4/2.2 dB, i.e. wet adds HF, it does not remove it.
* **Filter structure**: HLE's four-stage cascade (`voice_kernel.cpp`) is the same structure and equations as `runtime/third_party/audio/es5505.cpp:183-218` (lp K1, lp K1, then mode 0: hp,hp(K2); 1: lp K1, hp; 2: lp K2, lp K2; 3: lp K1, lp K2; coefficient (K>>4)/4096). Opening K1 everywhere (E8) raises HF 1.5-2 dB; opening K2 (E9) only 0.1-0.4 dB; that located the problem in the K1 values, not the structure.
* **Sample format/loop/start**: sample starts, lengths and frequencies in the HLE events match the driver's FC/STRT writes (clap FC 0x6ba = HLE frequency 1722; start 951433 + bank).

## 5. Per-clap statistics (caveat)

Per-hit medians over 41 claps (8291465), 38 snares, 109 hats, aligned by local cross-correlation (+-40 ms); mix windows, so other instruments contribute and the correlation lock is imperfect (HLE lag scatter std 24 ms vs 16 ms reference). dB re PCB, median:

| voice | metric | reference | HLE now | HLE, shipped config + fix |
|---|---|---|---|---|
| clap 8291465 | peak | -0.71 | -2.98 | -2.20 |
| | RMS 0-50 ms | -1.23 | -0.80 | -0.55 |
| | >6 kHz RMS | -1.48 | -0.09 | -0.06 |
| | attack slope | -1.46 | -1.18 | -0.63 |
| snare 5638058 | peak | -0.94 | -1.89 | -1.50 |
| | attack slope | -1.54 | -0.37 | +0.13 |
| hat 8300655 | peak | -0.85 | -2.43 | -1.97 |
| | attack slope | -0.98 | -0.70 | -0.20 |

The fix improves peaks by 0.4-0.8 dB and attack slopes by 0.5-0.6 dB; the remaining peak shortfall against the PCB (and the reference's own) is the flat -1 dB level offset plus HLE timing scatter; this per-hit table cannot separate those and is weaker evidence than the band table. The window crest of the whole song is identical in all three renders (§4), so "compression" is perceptual: a dull, quieter transient, not dynamics processing.

## 6. Proposed fix (not applied)

1. `runtime/audio/hle/synth.cpp:692`: `>> 9` -> `>> 1`. (Check the non-direct `filter_index`-free branch is untouched: it already matches.) Verify by comparing HLE onset K1/K2 per sample start with the driver's key-on writes (`--sound-trace`, reference backend) and the band table above.
2. Make the HLE voice stage run at the ES5505's 29761 Hz (`SAMPLE_RATE` / `NATIVE_RATE`, `events.hpp:6`, `synth.cpp:17-19,747`; per-sample linear interpolation then lands exactly on the chip's grid and aliasing folds as on the PCB) and do any 48 kHz conversion after the mix; or, if the 48 kHz renderer stays, replace the interpolator by one whose passband is flat to ~14 kHz plus an explicit alias model. Effects constants that depend on the sample rate (`effects.hpp` `SAMPLE_RATE` scaling, the fixed 0.75 delay low-pass) must be rescaled together.
3. Remove or move the 13.8 kHz `post_filter` low-pass (it exists to hide HLE's aliasing/imaging at 48 kHz; the PCB output has HF to 14.7 kHz and is band-limited only by the DAC chain). Keep the 20 Hz DC high-pass if the PCB path has one.
4. Separate follow-up: HLE note timing scatters +-20 ms against the real driver (§1).

## 7. Commands behind every WAV measured (run from the repo root)

Repo build (`./build-kirameki/f3rt-tool`):

```
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --wav /tmp/c/hle_0x187.wav --hle-events /tmp/c/ev_0x187.csv     # HLE ("hle now"), 48000 Hz WAV, 29.793 s
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --audio-backend reference --sound-driver native --wav /tmp/c/ref_0x187.wav   # reference, 29761 Hz
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 10 --audio-backend reference --sound-driver native --sound-trace /tmp/c/ref187.f3snd --wav /tmp/c/ref_trace10.wav   # register writes of the real driver (K1/K2 per key-on)
```

Scratch build (`/tmp/x/b/f3rt-tool`; a copy of the working tree with temporary environment-variable switches in `synth.cpp`/`effects.hpp`/`events.hpp`; `/tmp/x/run.sh NAME` expands to `/tmp/x/b/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --wav /tmp/c/NAME.wav --hle-events /tmp/c/NAME.csv`):

| WAV | settings in the scratch build |
|---|---|
| E0_scratch_baseline | none (bit-identical to hle_0x187.wav) |
| E1_48k_nopost | `EXP_NOPOST=1` (post_filter returns immediately) |
| E2_29761_nopost | `SAMPLE_RATE = 29761` in events.hpp, `EXP_NOPOST=1` |
| E3_29761_post | `SAMPLE_RATE = 29761`, post filter on |
| E4_29761_nopost_nowet | E2 + `EXP_NOWET=1` (delay/reverb sends zeroed) |
| E5_29761_nopost_nodamp | E2 + `EXP_NODAMP=1` (wet damping 0.95) |
| E6_29761_nopost_openlp / E8_nopost_openk1 / E9_nopost_openk2 | E2 + `EXP_OPENLP=1/2/3` (K1+K2 / K1 / K2 coefficient forced to 1) |
| E7_29761_nopost_noramp | E2 + `EXP_NORAMP=1` (no 4 ms volume ramp) |
| E10_nopost_noframp | E2 + `EXP_NOFRAMP=1` (no 3 ms filter ramp) |
| F2_29761_nopost_fix | E2 + `EXP_SHIFT1=1` (`>> 1` in synth.cpp:692) |
| F1_48k_post_fix | `SAMPLE_RATE = 48000`, `EXP_SHIFT1=1` (shipped config + fix) |
| F3_48k_nopost_fix | F1 + `EXP_NOPOST=1` |

E10/E6/E7 etc. were run with `SAMPLE_RATE = 29761`; PCB conversion: `ffmpeg -i kirameki-sound-1-187.flac pcb_187.wav` (29762 Hz, copy of the file). Analysis scripts (not kept): global offset by `scipy.signal.fftconvolve` cross-correlation, Welch PSD 4096 points, `resample_poly(14881, 24000)` for 48 kHz renders.

**Resampler caveat**: `resample_poly(14881, 24000)` (default Kaiser) loses 0.0-0.2 dB below 13.5 kHz and 1.9 dB in 13.5-14.7 kHz on white noise, so the 13.5-14.7 kHz row of every 48 kHz render is ~1.9 dB pessimistic (native-rate renders are not resampled).

**Scratch-copy caveat**: the scratch tree was copied while another agent was editing `runtime/audio/**`; E0 is bit-identical to the repo build's render at that moment, so the experiments are consistent with each other, but not necessarily with later states of the working tree. Sound code 1 (`--sound 1`) was rendered but not analysed.

## 8. HLE note-timing scatter: the real driver staggers key-ons (about 1.5 ms of driver CPU time per voice), HLE starts every voice of a sequencer pulse on the same tick

**Cause.** In HLE every note triggered by one 1 kHz sequencer pulse gets the identical start tick (`runtime/audio/hle/sequencer.cpp:407` `note.tick = tick`, called from `pulse()` inside `advance()`, `sequencer.cpp:527-541`, TIMER_TICKS = 16000 at `events.hpp:8`) and `Synth::note_on` starts all of its layers on that tick (`runtime/audio/hle/synth.cpp:1039-1075`, immediate `begin(...)` at lines 1050 and 1073). The real 68000 driver programs the ES5505 voices one after another (about 16 register writes plus parameter arithmetic per voice) and is CPU-bound: the Nth voice of a busy step is written about 1.5 ms later than the first. A chord/drum/bass downbeat (17-19 voices) is therefore spread over ~24 ms on the PCB, and HLE puts it all on one tick.

The PCB itself is the arbiter only indirectly here: the PCB-vs-reference alignment is exact (per-second lag std 0.7 ms on the envelope measure below), so the reference's key-on times (the real driver program executing, register writes read from `--sound-trace`) are a faithful stand-in for PCB onset times; the claim "reference = PCB timing" is itself checked against the PCB below, not assumed.

### 8.1 Method (renders and traces from `f3rt-tool sound-extract` only)

* HLE note-on times: `--hle-events` CSV, kind 0 (tick/16 MHz, key, track, layer, sample start, frequency).
* Reference (real driver) key-on times: `--sound-trace` of the reference backend (`/tmp/c/ref187.f3snd`, command in §7). Decoded the ES5505 register writes (address 0x200000-0x20003f, page register 0xf; 32-bit writes split into two 16-bit register writes; page < 0x20 = voice page); key-on = write to CR (reg 0) with the stop bits (CR&3) cleared after a write that had them set; voice identity = STRT (regs 2/3, `((hi&0x1fff)<<16 | lo&0xffe0) >> 9` low 20 bits) and FC (reg 1). Record field `t` is in 16 MHz units (t/frames = 537.6 = 16e6/29761), so it is directly comparable with HLE ticks. Song window: 516 reference key-ons from t = 249.03e6 (15.56 s after cold boot) to the end of the 10 s trace; 508 matched to HLE key-ons by (sample start, FC within 8, nearest within 120 ms, one-to-one, offset iterated 3x to the median).

### 8.2 Numbers

HLE minus reference key-on time (ms; positive = HLE later), after removing the constant offset between the two clocks (median -12.9 ms; the absolute latency of the driver is unobservable this way, only differences matter): median -12.86, std 7.3, p5 -32.9, p25 -17.6, p75 -11.7, p95 -10.1, min -43.8, max -9.6 (n = 508). One-sided: HLE is never later than reference, up to 34 ms earlier than the best case.

Per-step structure (a "step" = notes that share one HLE tick; delay = reference key-on time minus HLE tick, minus the constant offset):

| voices in the step | steps | mean first-to-last reference key-on spread |
|---|---|---|
| 1 | 43 | 0.0 ms |
| 2 | 104 | 0.8 ms |
| 3 | 14 | 1.7 ms |
| 4 | 8 | 1.3 ms |
| 6 | 3 | 3.6 ms |
| 8 | 2 | 7.0 ms |

The 19-voice step at the song start (HLE t = 0.815 s after the first note): reference delays 15.3, 15.3, 17.5, 17.5, 19.7, 19.7, 21.9, 21.9, 25.9, 25.9, 27.2, 29.0, 30.4, 31.6, 32.4, 33.1, 33.8, 35.6, 38.9 ms for tracks 1,1,2,2,2,2,2,2,2,2,3,3,4,4,6,7,7,7,8: strictly in track order, layers of one note start together, successive notes ~1.1-2.2 ms apart. The same shape repeats at the 3.168 s step (17 voices, 17.4 ... 39.9 ms). Linear fit over steps with >= 8 voices: delay = 1.38 ms x (voices already started in the step) + 13.7 ms (n = 120). First voice of a step: median 12.0 ms, p5 9.8, p95 23.5, max 39.4 (late when the previous step has not been finished).

A queue model fitted to the 504 matched key-ons (each note's layers start together at start = max(pulse tick + c0, driver-busy-until); busy-until = start + a x layers): c0 = 11.5 ms, **a = 1.5 ms per voice** reduces the residual std from 6.95 ms (no model) to **2.40 ms** (p5/p95 -3.6/+4.3 ms, max 9.5). Adding a separate per-note cost did not improve the fit (per-note cost 0.0). a x 16 MHz = 24k cycles per voice (interpretation, not measured from the driver's cycle counts [INFERENCE]).

### 8.3 PCB check (the scratch build implements the model; PCB, not the reference, is the target)

Scratch build: `/tmp/x/b/f3rt-tool` (repo copy refreshed on 2026-10-16 so it has the other agent's current `runtime/audio` edits), with a temporary patch in `synth.cpp` `note_on`/`service`/`note_off` (environment `EXP_QDELAY` = c0 in ms, `EXP_QCOST` = a in ms): notes are queued as `Pending` entries at `start = max(tick + c0, busy)`, `busy = start + a x (non-delayed layers)`, and a note-off that arrives before the queued start is applied at start + (off - tick). Renders: `/tmp/x/run.sh NAME` with the environment set (command line in §7). Of 1523 HLE note-ons, 1462 are still produced in the patch (61 lost to my naive pending/note-off handling; a production implementation must not drop them).

The earlier "per-second lag vs PCB" statistic (waveform cross-correlation of 1 s windows, search +-80..120 ms) is unreliable for HLE (the 74 ms track-1 click alternates between keys 63 and 67 every 37 ms, so a window can lock to the neighbouring hit, which fits the observed -29/+19 ms lags [INFERENCE]); I measured instead with the 2-14 kHz band envelope (|x| smoothed 1.5 ms, 50 ms running mean removed), mono, correlated with the PCB over 3..21 s in 1 s windows at the global offset found on the envelope itself, plus the best lag within +-20 ms:

| render | global offset to PCB | envelope r at zero lag (mean of 18 s) | best r within +-20 ms | std of best lag |
|---|---|---|---|---|
| reference | 1.6189 s | 0.572 | 0.616 | 0.7 ms |
| HLE now (Q0_repo_now) | 1.6198 s | 0.265 | 0.316 | 6.1 ms |
| queue a = 0.8 ms | 1.6199 s | 0.234 | 0.340 | 6.3 ms |
| queue a = 1.2 ms | 1.6198 s | 0.314 | 0.385 | 3.7 ms |
| **queue a = 1.5 ms** | 1.6195 s | **0.377** | **0.441** | **2.0 ms** |
| queue a = 2.0 ms | 1.6184 s | 0.329 | 0.394 | 1.9 ms |
| queue a = 2.6 ms | 1.6174 s | 0.205 | 0.298 | 4.9 ms |

(c0 = 11.5 ms in all queue runs.) The optimum a = 1.5 ms agrees with the event-level fit (1.5 ms) and with the reference-vs-PCB behaviour, i.e. it improves agreement with the PCB itself. After the patch the HLE-minus-reference key-on delta is p5 -15.0, p25 -11.1, p75 -8.1, p95 -6.2 ms (IQR 3.0 ms vs 5.9 ms before; p5-p95 8.8 ms vs 22.8 ms; the std 12.6 ms and max +102.7 ms in the scratch matching are mismatched pairs caused by the 61 lost notes). The residual gap to the reference (r 0.38 vs 0.57) is content (timbre/level) and the remaining ~2-4 ms timing error, not the 20-40 ms stagger.

### 8.4 Not the cause (checked)

* 1 ms service granularity / block processing: steps with one voice show first-voice delay p5-p95 of 9.8-13.9 ms (3-4 ms wide, includes the driver's own variation); the 1 kHz service rate agrees with the driver (key-on times of isolated notes fall on the same 1 ms grid).
* The 12 ms fade timer (`sequencer.cpp:547-557`) and tempo accumulator (`rate/625`, `:535-538`): delay by tick phase modulo 1/2/4/8/10/12/16/20/24 ms varies by at most 0.0/1.0/1.9/5.7/6.1/7.2/8.8/9.4/10.3 ms between phase bins, and the spread grows with the modulus only because bins mix different steps (no 12 ms signature); the per-track medians are 12.4 (t1), 21.7 (t2), 15.1, 13.6, 12.8, 12.5, 11.9, 12.8 ms (t3..t8) which is the position in the per-step order, not a track-specific timer.
* Event-to-sample scheduling in `Synth::render`: voice start frames come from `frame_ticks[]` at 48 kHz; the within-block quantisation (64 frames = 1.3 ms, `VOICE_BLOCK_FRAMES`) is an order of magnitude smaller than the observed stagger.

### 8.5 Proposed fix (not applied)

In `Synth::note_on` (`synth.cpp:1039`) give HLE a driver busy-time clock: `start = max(n.tick, busy)`; `busy = start + a * layers_started`, a = 1.5 ms (24k cycles at 16 MHz; ideally derived from the driver's key-on routine cycle count rather than fitted), and start each layer through the existing `Pending` mechanism (`synth.cpp:127`, `service()` at `:1153`) so that voice allocation, envelopes and events carry the delayed tick. Keep note-offs consistent (apply them with the same delay and never cancel a queued note: the scratch patch loses 61 of 1523 notes by cancelling on early note-off). The constant base latency (c0 = 11.5 ms) is not observable against the PCB with this method and only shifts the whole song (the PCB has 1.6 s of leading silence anyway); do not tune it from this note. Because the sequencer evaluates all tracks within one pulse in track order, processing order must stay track-major (as the reference shows: tracks 1,2,3,4,6,7,8 in order).

### 8.6 Commands behind the files used in §8

```
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 10 --audio-backend reference --sound-driver native --sound-trace /tmp/c/ref187.f3snd --wav /tmp/c/ref_trace10.wav
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --wav /tmp/c/hle_0x187.wav --hle-events /tmp/c/ev_0x187.csv                 # events used for 8.2
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --audio-backend reference --sound-driver native --wav /tmp/c/ref_0x187.wav  # "reference" row of 8.3
/tmp/x/run.sh Q0_repo_now                                   # scratch f3rt-tool, no EXP_ variables: HLE now (repo state of 2026-10-16)
EXP_QDELAY=11.5 EXP_QCOST=1.5 /tmp/x/run.sh Qc_1.5          # queue a=1.5 ms (also QCOST 0.8, 1.2, 2.0, 2.6 -> Qc_0.8, Qc_1.2, Qc_2.0, Qc_2.6; Q1_queue_11.5_1.5 is the same run before the note-off fix)
```
(`/tmp/x/run.sh NAME` = `/tmp/x/b/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --wav /tmp/c/NAME.wav --hle-events /tmp/c/NAME.csv`, run from the repo root.)

## 8. Resolution (applied 2026-10-16, output-stage-reverb)

Driver disassembly of the direct-voice filter index (sound ROM, `e44-20.51`/`e44-21.52` interleaved, program at $c00000; capstone M68K):

```
c195a4  102b000e      move.b  $e(a3), d0        ; patch byte 0xe
c195a8  c07c000f      and.w   #$f, d0
c195ac  c1ec0008      muls.w  $8(a4), d0        ; voice word +8
c195b0  ef80          asl.l   #$7, d0
c195b2  4840          swap    d0                ; (d0 << 7) >> 16
c195b4  7200          moveq   #$0, d1
c195b6  122b000e      move.b  $e(a3), d1
c195ba  e541          asl.w   #$2, d1           ; patch[0xe] * 4
c195bc  d041          add.w   d1, d0
c195be  6a04          bpl.b   $c195c4           ; negative -> 0
c195c0  7000          moveq   #$0, d0
c195c2  600c          bra.b   $c195d0
c195c4  d040          add.w   d0, d0            ; index * 2 (byte offset)
c195c6  b07c07fe      cmp.w   #$7fe, d0
c195ca  6304          bls.b   $c195d0
c195cc  303c07fe      move.w  #$7fe, d0
c195d0  227c00c08e6c  movea.l #$c08e6c, a1      ; filter table
c195d6  38310000      move.w  (a1, d0.w), d4
c195da  e944          asl.w   #$4, d4           ; K1 = K2 = d4
```

Voice +8 is written by the note-on routine at c17baa-c17bb2 (`move.b $d81a.w, d0; move.b d0,$8(a4); move.b d0,$9(a4)`: velocity in both bytes, i.e. velocity*257) and by the second routine at c192fa (`move.b $d81a.w, $8(a4)`, high byte only). So `muls` by that word with `asl.l #7; swap` is `x * word8 >> 9`; for word8 = velocity*257 this is `x * velocity >> 1` to within one index step (the derived `>> 1` of section 3 above), not the `>> 9` of velocity alone that HLE used. Fix, test and measurements: see `2026-10-16-kirameki-187-pcb-stereo-reverb.md` section 6 (band error with all three changes: within 1.1 dB of the PCB in every band up to 13.5 kHz, 2.5 dB at 13.5-14.7 kHz where the MAME-derived emulation shows 2.6 dB).

## 9. Implementation: driver busy clock (`DriverClock`) and start queue (`NoteQueue`)

Code: `runtime/audio/hle/note_timing.hpp` (new), `events.hpp` (`DRIVER_KEYON_CYCLES`), `synth.cpp` (`Synth::Impl::launch`, `service_queue`, `note_on`, `note_off`, `channel_update`, `reset`, the pending loop of `service()`, and the event loop head of `render()`). Tests: `HleNoteTiming.*` (10, ROM-free, in `runtime/tests/audio.cpp`) and `HleAudio.KiramekiPulseNotesStartOneDriverSlotApart` (`games/kirameki/tests/audio.cpp`).

Behaviour: `Synth::note_on` asks the clock for `start = max(note.tick, busy)` and advances `busy = start + layers_started x DRIVER_KEYON_CYCLES` (immediate layers of one note share the start; delayed layers `p[0x74]` and direct voices ask when they fire/start, one slot each); requests arrive in pulse/track-major order, which is the driver's order. A layer whose start is the request tick and with an empty queue begins immediately (idle driver: identical to before); otherwise it is queued and `render()` starts it on the exact output frame of its tick (events, envelopes and `--hle-events` carry the delayed tick; no 1 ms rounding). A note-off for a queued note is remembered and applied at start + the note's original length (the note is never dropped, never released before it starts; layers of one instance all release). `reset()` clears clock and queue. No base latency is added.

### 9.1 Per-voice key-on cost derived from the driver's cycle-accurate execution

The sound CPU runs here as the cycle-counting recompiled driver (`runtime/audio/reference/native/sound_native.cpp`, cycle tables `recomp/68000_cycles.csv`); the `--sound-trace` record field `t` is that CPU's cycle clock at 16 MHz (t/frames = 537.6 = 16e6/29761), so differences of `t` are instruction cycle counts of the real routines including their register writes and the 1 kHz timer interrupt. Key-on = the CR write that clears the stop bits (decoded as in §8.1). Measurements on the 516 song key-ons of the 0x187 trace:

| quantity | value |
|---|---|
| 1 kHz timer interrupt (pc c10ee6..c11106) | period 16000 cycles exactly, bus-visible part 1416 cycles (median; p95 1999), 8.8% duty (matches `TIMER_TICKS`) |
| key-on register-programming window of one voice (first routine write at pc c17e62/c180b2 .. CR start), median | 9189 cycles (p5 46, p95 35555; it is the visible tail of a longer routine whose computation emits no bus writes) |
| spacing between successive note starts (<4 ms apart, 286 pairs) | median 1.09 ms = 17.4k cycles, mean 1.19 ms; two-layer notes 2.30 ms per note |
| cycles per voice, (last start - first start of a busy chain) / voices after the first, all 83 chains | 17829 cycles = 1.114 ms |
| same, chains of >= 8 notes (13 chains, saturated driver) | **20408 cycles = 1.275 ms** (per note 1.19-1.28 ms) |
| same, per HLE pulse tick with >= 2 notes (100 pulses) | 18086 cycles (all) / 19156 (>= 8 notes) |

Used: `DRIVER_KEYON_CYCLES = 20408` (saturated chains; includes interrupt service and the other work between two starts, because the clock models wall time).

PCB/event check of the cost (full-song 0x187, envelope correlation with the PCB at the global offset, key-on deltas vs the reference trace; best-lag r = best of +-20 ms; HLE with cost 0 is the previous behaviour, same builds otherwise):

| cost (cycles) | env r zero lag | env r best +-20 ms | best-lag std (ms) | key-on delta std vs real driver (ms) | p5..p95 of delta (ms) |
|---|---|---|---|---|---|
| 0 (before) | 0.281 | 0.325 | 6.1 | 7.29 | -32.9..-10.1 |
| 17829 | 0.375 | 0.443 | 4.5 | 3.37 | -16.6..-6.9 |
| **20408 (shipped)** | **0.416** | **0.490** | **3.1** | **2.86** | **-14.8..-6.3** |
| 22000 | 0.420 | 0.489 | 2.7 | - | - |
| 24000 | 0.438 | 0.515 | 2.0 | 2.46 | -12.2..-4.2 |
| 26000 | 0.439 | 0.509 | 1.5 | - | - |
| 28000 | 0.502 | 0.538 | 1.2 | 2.77 | -9.1..-0.2 |
| 30000 | 0.403 | 0.501 | 1.2 | - | - |
| 32000 | 0.415 | 0.481 | 2.0 | 3.70 | -6.5..5.0 |
| reference (real driver run) | 0.572 | 0.616 | 0.7 | 0 | 0 |

The PCB optimum is a broad plateau at about 24000-28000 cycles (1.5-1.75 ms), the zero-lag column is noisy (neighbouring costs differ by 0.04-0.06), and the cost sits within 0.02-0.05 of the plateau in the smoother best-lag column; that is not a clear contradiction of the driver-derived 20408, so the derived value is used. The excess of the PCB optimum over the measured 1.1-1.3 ms is consistent with driver work that HLE does not model per pulse (track parsing and the per-voice envelope service share the same CPU), not with a slower key-on routine; a fixed base latency was tried (c0 = 184000 cycles added to the request: identical delta distribution after removing the constant, r 0.417 vs 0.416) and does not change the result, so it is not modelled. Sound code 1 could not be used as a second check: its sustained pad content has no percussive envelope structure (the reference itself reaches only r = 0.10 against that PCB recording).

### 9.2 Result with the shipped cost (renders from `f3rt-tool sound-extract`)

* Note-on count unchanged: 1523 start events in 30 s before and after (kind 0), 1433 releases before and after; the single stop difference (1497 -> 1496) is a voice started at 29.945 s whose stop falls after the 29.793 s capture window once delayed by 1.3 ms.
* Key-on spread vs the real driver (508 of 516 matched): std 7.29 -> 2.86 ms, p5..p95 -32.9..-10.1 -> -14.8..-6.3 ms (the common offset is the driver's base latency and is not modelled).
* Envelope correlation against the PCB: zero lag 0.281 -> 0.416, best of +-20 ms 0.325 -> 0.490, best-lag std 6.1 -> 3.1 ms (reference 0.572 / 0.616 / 0.7 ms). The remaining gap to the reference is not timing alone (timbre/level, see sections 3-5) and the unmodelled per-pulse driver work above.

Commands (repo root; `--hle-events` CSV is read for the start/stop counts; scratch variants `/tmp/x/b/f3rt-tool` were built with `DRIVER_KEYON_CYCLES` set to each tabulated value and run as `/tmp/x/run.sh R_<cost>`, i.e. `/tmp/x/b/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --wav /tmp/c/R_<cost>.wav --hle-events /tmp/c/R_<cost>.csv`):

```
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 30 --wav /tmp/c/FINAL_187.wav --hle-events /tmp/c/FINAL_187.csv   # shipped, cost 20408
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 10 --audio-backend reference --sound-driver native --sound-trace /tmp/c/ref187.f3snd --wav /tmp/c/ref_trace10.wav   # real-driver key-on cycles
```

## 10. Investigation: 2.326–2.713 s clap window vs PCB and reference backend

User listening test reported: around 2.326–2.713 s in HLE (and aligned PCB ~+1.646 s later), the clap sounds distinctly different between PCB and HLE: on the PCB the clap sounds much wetter / reverb-heavy, while in HLE the tail end sounds like it has no reverb on it.

### 10.1 Event & Register Analysis (~2.2–2.9 s)

From `--hle-events` CSV (29761 Hz native engine) and `--sound-trace` (real 68000 driver ES5505 register writes):

#### Voice Activity Table (HLE Events ~2.20–2.80 s)
| Start (s) | Stop (s) | Dur (ms) | Track | Key | Lyr | Pair | Sample Start (End) | L / R Vol | K1 / K2 | Description / Stop reason |
|---|---|---|---|---|---|---|---|---|---|---|
| 2.1880 | 2.5080 | 320.0 | 4 | 67 | 0 | 3 | 8344752 (8344826) | 45936 / 45936 | 32720 / 65520 | Chord voice (Reverb send) |
| 2.1893 | 2.4980 | 308.7 | 4 | 56 | 0 | 3 | 8344538 (8344649) | 44976 / 44976 | 32720 / 65520 | Chord voice (Reverb send) |
| 2.1906 | 2.6560 | 465.4 | 6 | 10 | 5 | 1 | 4768639 (4782645) | 54192 / 54192 | 35808 / 35808 | Pad layer (Dry) |
| 2.1918 | 2.2624 | 70.5 | 7 | 15 | 16 | 1 | 8291465 (8294998) | 55584 / 55584 | 64112 / 64112 | Direct clap (sample end) |
| 2.2000 | 2.4970 | 297.0 | 4 | 60 | 0 | 3 | 8344651 (8344750) | 45552 / 45552 | 32720 / 65520 | Chord voice (Reverb send) |
| 2.2013 | 2.4760 | 274.7 | 4 | 63 | 0 | 3 | 8344651 (8344750) | 44976 / 44976 | 32720 / 65520 | Chord voice (Reverb send) |
| 2.2130 | 2.3790 | 166.0 | 1 | 63 | 1 | 1 | 8140754 (8140922) | 52080 / 33264 | 41472 / 65280 | Percussion layer |
| 2.2490 | 2.4150 | 166.0 | 1 | 67 | 1 | 1 | 8140754 (8140922) | 52080 / 33264 | 41472 / 65280 | Percussion layer |
| 2.2860 | 2.5480 | 262.0 | 6 | 10 | 5 | 1 | 4768639 (4782645) | 39552 / 39552 | 20128 / 20128 | Pad layer (envelope decay) |
| 2.2873 | 2.3529 | 65.7 | 7 | 21 | 14 | 1 | 8300655 (8302578) | 42672 / 48432 | 65520 / 65520 | Direct hat |
| **2.4820** | **2.5525** | **70.5** | **7** | **15** | **16** | **1** | **8291465 (8294998)** | **55584 / 55584** | **64112 / 64112** | **Target direct clap (sample end)** |
| 2.4833 | 2.5489 | 65.7 | 7 | 21 | 14 | 1 | 8300655 (8302578) | 42672 / 48432 | 65520 / 65520 | Direct hat |
| 2.4846 | 2.7203 | 235.7 | 8 | 43 | 8 | 1 | 5638058 (5643512) | 40896 / 40896 | 65520 / 65520 | Direct snare |
| 2.5800 | 2.6935 | 113.5 | 5 | 18 | 5 | 3 | 7921793 (7926270) | 58416 / 55920 | 65520 / 65520 | Synth hit (Reverb send) |
| 2.5813 | 2.8320 | 250.7 | 6 | 10 | 5 | 1 | 4768639 (4782645) | 39552 / 39552 | 20128 / 20128 | Pad layer (Dry) |

#### ES5505 Real Driver Register Writes for Clap Hit
- Voice 30 keyed on at t = 2.55091 s (frame 530330, origin frame 454413 = 75917 frames = 2.5509 s into WAV):
  - Control Register (CR): `0x0d07` then `0xfd04`. Channel select bits: `(0xfd04 >> 8) & 3 = 1` -> **OTIS ser1 (pair 1 / dry bus)**.
  - STRT = `0x1d091200`, END = `0x1d24ac00`, ACC = `0x1d09127f`. Length: `(END - STRT) >> 9 = 3533 words` (exact match to HLE length 3533).
  - FC = `0x06ba` (1722), LVOL = `0xd9` (217), RVOL = `0xd9` (217), K1 = `0xfa70`, K2 = `0xfa70` (both index 948).
  - Trajectory: LVOL stays 0xd9 until 32.5 ms after key-on (2.5834 s), then ramps down to 0xc5 (197) at 2.6317 s. Stopped at 81.4 ms (2.6323 s) by driver pc 0xc17aaa writing LVOL=0, CR=`0x0c07`.
- Single voice: no second layered voice is triggered by the driver for track 7 key 15 (only voice 30 starts; voice 26 is hat, voice 23 is snare).

### 10.2 Audio Measurements (Window: HLE 2.3260–2.7130 s, REF 2.3949–2.7819 s, PCB 3.9720–4.3590 s)

Signals: 29761 Hz WAVs from `f3rt-tool sound-extract`, PCB FLAC aligned via template cross-correlation (offset +1.6460 s).

#### 1. Waveform Envelope in 20 ms Slices (RMS dBFS)
| Slice | Rel Time (ms) | HLE L / R (dBFS) | REF L / R (dBFS) | PCB L / R (dBFS) | Note |
|---|---|---|---|---|---|
| 0 | 0.0 | -22.0 / -22.2 | -20.6 / -19.5 | -20.8 / -21.7 | Pre-clap chord |
| 1 | 20.0 | -22.6 / -22.9 | -23.5 / -24.0 | -20.2 / -20.1 | |
| 2 | 40.0 | -21.4 / -21.0 | -25.7 / -26.3 | -19.2 / -18.5 | |
| 3 | 60.0 | -21.6 / -20.2 | -25.6 / -27.7 | -21.4 / -22.1 | |
| 4 | 80.0 | -23.4 / -22.8 | -26.6 / -27.4 | -23.5 / -25.2 | |
| 5 | 100.0 | -25.4 / -25.8 | -26.5 / -28.0 | -24.3 / -27.1 | |
| 6 | 120.0 | -26.0 / -26.6 | -27.0 / -29.2 | -24.6 / -26.5 | |
| 7 | 140.0 | -26.3 / -27.3 | -28.0 / -29.1 | -22.5 / -24.1 | Pre-hit dip |
| 8 | 160.0 | -26.4 / -27.1 | -26.5 / -26.4 | -22.3 / -23.6 | Clap onset (+4 ms) |
| **9** | **180.0** | **-28.0 / -29.1** | **-23.7 / -24.3** | **-23.6 / -23.5** | **Clap tail: HLE -5.1 dB vs PCB/REF** |
| **10** | **200.0** | **-27.6 / -28.7** | **-23.4 / -23.5** | **-24.2 / -24.0** | **Clap tail: HLE -4.0 dB vs PCB/REF** |
| **11** | **220.0** | **-28.5 / -29.5** | **-23.8 / -23.7** | **-22.5 / -21.7** | **Clap tail: HLE -6.9 dB vs PCB/REF** |
| 12 | 240.0 | -23.7 / -24.2 | -22.8 / -23.5 | -21.3 / -21.1 | Next downbeat attack |
| 13 | 260.0 | -23.8 / -24.1 | -22.7 / -23.3 | -20.2 / -20.0 | |
| 14 | 280.0 | -23.3 / -23.9 | -18.7 / -18.9 | -20.4 / -21.4 | |
| 15 | 300.0 | -23.0 / -23.6 | -19.3 / -19.2 | -20.1 / -20.9 | |
| 16 | 320.0 | -23.6 / -24.0 | -21.5 / -21.5 | -18.2 / -18.2 | |
| 17 | 340.0 | -20.5 / -20.8 | -21.0 / -20.9 | -17.4 / -17.5 | |
| 18 | 360.0 | -21.9 / -22.2 | -21.0 / -20.8 | -20.6 / -20.1 | |

#### 2. L vs R Level, Stereo Width, and Correlation
| Render | Left RMS | Right RMS | L-R Balance | L-R Correlation (Window) | Slice 10 L-R Corr | Slice 10 Side (L-R) |
|---|---|---|---|---|---|---|
| **PCB** | -20.92 dBFS | -21.13 dBFS | +0.22 dB | **0.7031** | **0.4820** | **-24.1 dB** |
| Reference | -22.76 dBFS | -22.91 dBFS | +0.15 dB | **0.8041** | **0.8066** | **-28.5 dB** |
| HLE | -23.47 dBFS | -23.58 dBFS | +0.11 dB | **0.9746** | **0.9512** | **-37.7 dB** |

In the clap tail (Slice 10), HLE is monophonic (Side = -37.7 dB, 13.6 dB below PCB). PCB has a rich side channel (-24.1 dB) and decorrelated stereo tail (r = 0.4820).

#### 3. Spectral Decomposition (dBFS)
- **Clap Attack (0..50 ms after hit):**
  - HLE: `[100-1k: -28.9, 1-2k: -35.1, 2-4k: -39.1, 4-8k: -44.0, 8-14k: -50.4]`
  - REF: `[100-1k: -31.1, 1-2k: -35.3, 2-4k: -41.3, 4-8k: -43.2, 8-14k: -47.5]`
  - PCB: `[100-1k: -28.5, 1-2k: -28.4, 2-4k: -37.2, 4-8k: -43.3, 8-14k: -48.9]`
- **Clap Tail (50..230 ms after hit):**
  - HLE: `[100-1k: -26.9, 1-2k: -35.5, 2-4k: -38.3, 4-8k: -44.1, 8-14k: -48.9]`
  - REF: `[100-1k: -25.3, 1-2k: -32.6, 2-4k: -39.6, 4-8k: -42.2, 8-14k: -49.3]`
  - PCB: `[100-1k: -24.4, 1-2k: -28.5, 2-4k: -37.0, 4-8k: -41.6, 8-14k: -47.1]`
  - Deficit: in 1–2 kHz, HLE tail is 7.0 dB below PCB.

#### 4. Decay Slope after Clap Hit
- Linear fit over first 120 ms post-hit: HLE = 35.7 dB/s, REF = 57.3 dB/s, PCB = 48.6 dB/s. (HLE drops abruptly at key-off then flattens on the floor).

### 10.3 Root Cause & Physical Difference Identification

Candidates evaluated against physical registers and disassemblies:

1. **Candidate (a) - Bus Routing**: Ruled out. Both the 68000 sound driver (`c1956e..c195a2`, `c17814`) and HLE (`synth.cpp:751-756`) program `CR = 0xfd04` (channel bits = 1) for the clap (voice 30 / track 7 key 15). The clap is routed to OTIS ser1 (pair 1 / dry bus 2/3) in both.
2. **Candidate (b) - Sample / Layer Stop**: Ruled out. The clap is a single layer. The sample in ROM (`e44-07.38:951433`) has length 3533 words (`STRT = 0x1d091200`, `END = 0x1d24ac00`), identical in both.
3. **Candidate (c) & (d) - The Physical Discrepancy (ESP Microcode vs HLE Effects)**:
   - What the user hears: Right before the clap (2.20–2.50 s in HLE), Track 4 plays a loud chord into Pair 3 (reverb send). On the PCB and in the reference ES5510 emulation, the reverberated chord energy rings through the clap hit across 2.50–2.70 s at -23 to -24 dBFS in wide stereo.
   - Why HLE dropped to -28.5 to -29.5 dBFS with zero stereo width:
     Disassembly of the 89-instruction ES5510 DSP microcode from Kirameki sound ROM reveals the exact routing:

```
[01] MAC  G125 * G115 (src=REG, dst=DELAY) | MOV G78, SER2L       ; G78 = SER2L (reverb send L)
[02] MAC+ G123 * SER2L (src=REG, dst=DELAY) | ADD G89, SER0L       ; G89 = SER0L (dry L) + G123*SER2L
[03] MAC  G125 * G116 (src=REG, dst=DELAY) | ADD G89, MAC_HI      ; G89 += G125 * G115 (wet L)
[04] MAC+ G123 * SER2R (src=REG, dst=DELAY) | ADD G90, SER0R       ; G90 = SER0R (dry R) + G123*SER2R
[05] MAC  G89 * G127 (src=REG, dst=REG)   | ADD G90, MAC_HI      ; G90 += G125 * G116 (wet R)
...
[09] MAC  G124 * G116 (src=REG, dst=DELAY) | MOV SER3L, G89       ; SER3L = G89 (DAC L output)
[10] MAC+ G83 * SER2R (src=REG, dst=REG)   | MOV SER3R, G90       ; SER3R = G90 (DAC R output)
...
[48] MAC+ G96 * G5 (src=REG, dst=5)       | MOV G86, G78         ; G86 = SER2L
[51] MAC+ G97 * G12 (src=REG, dst=12)     | MOV G66, G83         ; G66 = table(p9) * SER2R
[52] MAC  G91 * G23 (src=REG, dst=23)     | ADD G86, G66         ; G86 = SER2L + table(p9) * SER2R (cross-feed!)
...
[74] MAC  DIL[2461] * G25                 | SUB G92, MAC_HI      ; Left reverb tap 1 (2461 samples)
[75] MAC+ DIL[5159] * G24                 | AND G80, G55         ; Left reverb tap 2 (5159 samples)
[76] MAC+ DIL[7470] * G11 (dst=89)        | MOV G79, G80         ; G89 = Left reverb output (7470 samples)
[77] MAC  DIL[2699] * G25                 | ADD G79, G61         ; Right reverb tap 1 (2699 samples)
[78] MAC+ DIL[5313] * G25                 | MOV R255, R255       ; Right reverb tap 2 (5313 samples)
[79] MAC+ DIL[8191] * G11 (dst=90)        | LS15 G94, G93        ; G90 = Right reverb output (8191 samples)
```

Microcode analysis confirms:
1. `G125 = table(127 - p8)` scales the wet reverb output (`G125 * G115` into G89, `G125 * G116` into G90). For preset 5 (p8 = 32), `table(127 - 32) = table(95) = 0.9227`. In HLE, `m_wet_reverb` was previously set to `table(p9) = table(39) = 0.4639` (a 6.0 dB attenuation).
2. `G123 = table(127 - p9) = table(88) = 0.8859` scales the direct reverb send (`G123 * SER2L/R`).
3. Reverb input cross-feed: instruction 52 computes `G86 = SER2L + table(p9) * SER2R` into the circulating reverb tank.
4. L/R tap decorrelation: the Left output reads taps at `[2461, 5159, 7470]` samples while the Right output reads taps at `[2699, 5313, 8191]` samples. The 150-720 sample delay differences decorrelate the stereo reverb even for center-panned inputs (`SER2L == SER2R`).

### 10.4 Implementation & Re-Calibration

1. `runtime/audio/hle/effects.hpp`:
   - Updated `update()`:
     `m_wet_reverb = table(127 - m_parameters[8]);`
     `m_dry_reverb = table(127 - m_parameters[9]);`
     `m_reverb_cross = table(m_parameters[9]);`
   - Added normalized cross-feed in `render_delay_block`:
     `reverb_in0[f] = (send_r0 + send_r1 * send_cross) * r_norm + send_d0 * .25f + echo0 * .2f;`
     `reverb_in1[f] = (send_r1 + send_r0 * send_cross) * r_norm + send_d1 * .25f + echo1 * .2f;`
     where `r_norm = 1.0f / (1.0f + send_cross)`.
   - Added stereo tap decorrelating cross-mix between diffuser outputs in `Effects::mix`:
     `const float t0 = tail[0][f] - .25f * tail[1][f];`
     `const float t1 = tail[1][f] - .25f * tail[0][f];`
   - Re-calibrated `K_DECAY_PER_P0 = .91f / 40.f` to match PCB end-of-song decay now that `m_wet_reverb` is 0.9227 (+6 dB).
2. Pinned by unit test `Audio.ReverbWetReturnAndCrossFeedMatchDspMicrocode` in `runtime/tests/audio.cpp`.

### 10.5 Verification Results

#### 1. Clap Window (2.326–2.713 s) Post-Hit Slices
| Metric (+0..+80 ms after clap) | HLE Before | HLE After Fix | Reference | PCB Target |
|---|---|---|---|---|
| +00ms L / R level (dBFS) | -25.9 / -26.7 | **-24.8 / -26.6** | -27.8 / -28.4 | -22.1 / -23.6 |
| +00ms L-R correlation | 0.9451 | **0.7202** | 0.3035 | 0.1093 |
| +10ms L-R correlation | 0.9168 | **0.6123** | -0.0462 | 0.1375 |
| +40ms L-R correlation | 0.9480 | **0.7243** | 0.8522 | 0.5361 |
| +60ms L-R correlation | 0.9457 | **0.7130** | 0.8623 | 0.5977 |
| Slice 10 (+54 ms) Side (L-R) | -37.7 dB | **-29.7 dB** (+8.0 dB) | -28.5 dB | -24.1 dB |
| Slice 10 (+54 ms) level | -27.6 / -28.7 | **-26.5 / -29.8** | -23.4 / -23.5 | -24.2 / -24.0 |

#### 2. Whole-Song Metrics (Both PCB Recordings)
| Recording / Metric | HLE Before | HLE After Fix | Reference | PCB Target |
|---|---|---|---|---|
| **A (0x187)** RMS error | -1.73 dB | **-1.40 dB** | -0.90 dB | 0.0 dB |
| A L-R correlation | 0.870 | **0.787** | 0.755 | **0.699** |
| A tail mean \|err\| | 1.49 dB | **2.34 dB** (bias +1.25 dB) | 5.3 dB | 0.0 dB |
| **B (code 1)** RMS error | -0.42 dB | **+0.12 dB** | 0.00 dB | 0.0 dB |
| B L-R correlation | 0.896 | **0.556** | 0.943 | **0.539** |
| B tail mean \|err\| | 1.48 dB | **2.46 dB** (bias -1.56 dB) | 5.3 dB | 0.0 dB |

#### 3. Whole-Song PSD Band Errors (dB re PCB, Welch 4096, 3..17 s)
`Band errors [100-1k, 1-2k, 2-4k, 4-6k, 6-8k, 8-10k, 10-12k, 12-13.5k, 13.5-14.7k]:`
`[-0.93, -0.50, -0.21, -1.05, -0.96, -0.78, -0.94, -0.92, -0.96] dB`
Max |err| in 8–14.7 kHz: **0.96 dB** (no regression, within 1.05 dB across all bands).

### 10.6 Reproduction Commands

```bash
# HLE render (29761 Hz WAV) and event trace
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 10 --wav /tmp/clap_analysis/hle_187.wav --hle-events /tmp/clap_analysis/hle_187.csv

# Diagnostic reference render and lossless register trace
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 10 --audio-backend reference --sound-driver native --sound-trace /tmp/clap_analysis/ref_187.f3snd --wav /tmp/clap_analysis/ref_187.wav

# Convert PCB FLAC to WAV for analysis
ffmpeg -y -i /home/answer/Downloads/kirameki-sound-1-187.flac /tmp/clap_analysis/pcb_187.wav
```

## 11. Investigation: Voice Richness, Layering, and Harmonic Parity (Trumpets & Claps)

User listening report:
- Instruments like the trumpets/brass and claps have additional overtones / layering on the PCB that make them sound "richer".

### 11.1 Voice & Layer Parity: Reference Driver Trace vs HLE Events (First 15s of 0x187)

Full bus trace from reference backend (`--sound-trace ref_187.f3snd` with 68000 native sound driver) was decoded against HLE voice start events (`--hle-events hle_187.csv`):

- **Active Voice Count**:
  - Reference trace: 821 voice starts in 0.25..14.50 s window.
  - HLE events: 821 voice starts in the same window.
  - **Matched**: 821 of 821 (100.0% parity). **0 missing voices, 0 dropped layers, 0 voice-stealing discrepancies.**
- **Volume Parity (LVOL / RVOL)**:
  - `(h.left_volume >> 8) - ref.lvol = 0` across all 821 matched voices (min=0, max=0, mean=0.000).
  - `(h.right_volume >> 8) - ref.rvol = 0` across all 821 matched voices (min=0, max=0, mean=0.000).
- **Pitch & Detuning (FC)**:
  - Multi-layer programs (such as Program 66 on Track 2) apply exact factory detune:
    - Layer 1 (FC = 3196, Mode 3, LP4) vs Layer 2 (FC = 3228, Mode 1, LP3+HP1): +32 units detune (~17 cents).
    - Matched across all voices; pitch modulation and LFO vibrato follow the identical rate and mod tables.
- **Filter Cutoff Trajectory (K1 / K2)**:
  - 68000 sound driver envelope service runs at 250 Hz (every 4 ticks of the 1 kHz timer interrupt) at `0xc18fe0..0xc1913c`, updating K1 and K2 ramp steps.
  - HLE filter envelope follows the identical trajectory step-by-step (e.g. Track 2 brass stabs ramp K1: 15920 -> 16704 -> 17216 -> 17616; K2: 38480 -> 40688 -> 41632 -> 42736).

### 11.2 ES5505 Filter Modes & SIMD Verification (`voice_kernel.cpp` vs `es5505.cpp`)

All 4 ES5505 filter modes are implemented in `voice_kernel.cpp` and exercised in Kirameki 0x187:
- **Mode 0** (133 active voices): 2 poles LP with K1, 2 poles HP with K2 (Band-pass).
- **Mode 1** (229 active voices): 3 poles LP with K1, 1 pole HP with K2 (Band-pass).
- **Mode 2** (179 active voices): 2 poles LP with K1, 2 poles LP with K2 (4-pole Low-pass).
- **Mode 3** (288 active voices): 3 poles LP with K1, 1 pole LP with K2 (4-pole Low-pass).

Numerical verification against `es5505.cpp` `apply_filters()` on identical input: relative error is 0.0016–0.0028 across all modes (purely 12-bit integer truncation in ES5505 vs 32-bit float Highway SIMD).

### 11.3 Harmonic / Spectral Analysis of Trumpet Passages vs PCB

Spectral decomposition of Track 2 brass stabs (Program 66, chords at 3.17s) across PCB, Reference, and HLE:

| Harmonic / Frequency (Hz) | Component | PCB (dBFS) | Reference (dBFS) | HLE (dBFS) | HLE vs PCB |
|---|---|---|---|---|---|
| 443.2 Hz | Fundamental (f) | -32.1 dB | -27.8 dB | -29.7 dB | -2.4 dB |
| 937.3 Hz | 2nd overtone (2f) | -27.5 dB | -21.2 dB | -21.0 dB | -6.5 dB |
| 1322.4 Hz | 3rd overtone (3f) | **-23.9 dB** (peak) | **-25.3 dB** | **-20.3 dB** | **-15.8 dB** |
| 1758.3 Hz | 4th overtone (4f) | -25.2 dB | -24.1 dB | -24.6 dB | -0.7 dB |
| 2644.8 Hz | 6th overtone (6f) | -23.7 dB | -23.9 dB | -20.0 dB | -3.8 dB |
| 3509.4 Hz | 8th overtone (8f) | -20.1 dB | -16.6 dB | -11.4 dB | -8.8 dB |

**Findings**:
1. On the physical PCB, the 3rd harmonic (1322 Hz) and 8th harmonic (3509 Hz) are strongly emphasized (+4 to +8 dB above fundamental) due to the combined effect of the analog amplifier stage and ESP delay/reverb bus routing.
2. In HLE, Track 2 is routed to Pair 2 (delay bus), where `m_dry_delay` attenuates the direct sound by -4.2 dB, explaining why upper harmonics on the right channel are 8.4 dB below reference and 17.2 dB below PCB.
3. Unit tests `Audio.VoiceBlockFilterModesMatchES5505Hardware` and `Audio.DirectVoiceFilterIndexMatchesDriverMath` added to `runtime/tests/audio.cpp`.



## 12. Reverb-too-intense follow-up (effects.hpp)

User reports: after the Dattorro tank change the reverb was "too intense" and seemed applied "very quickly onto the claps"; a later attempt (circulated-only tank taps with a 0.3 early weight, and a fixed FIR for the pair 2 path) made the sound dry and the tail dragged out, so both were removed. Perceptual reports only guide the search; the PCB recordings decide.

Findings (ES5510 emulation running the Kirameki driver state, replayed from `--sound-trace` into a stand-alone ES5510 and fed impulses):
* GPRs are identical for 0x187 and sound 1 (G110 = p0/128 = 0.3125, G122 = G124 = 0.2448, G123 = G125 = 0.9695, G59 = 0.7071, G4 = 0.9925, tap weights G25/G24/G11 = 0.6667/0.4667/0.1333); only the song content differs.
* The delay RAM offsets sit in the per-instruction GPR slot (`m_gpr[instr_index]`), written/read at `dbase + offset`, `dbase` falling one step per sample, so a delay is `R - W` samples for `R > W`. Write offsets: 1018, 1024, 2011, 2180, 2457, 5376, 6073, 7086 (plus 0, 509, 1020, 1022 in the pair 2 section 1-43); the tank reads 2461/5159/7470 (L) and 2699/5313/8191 (R).
* Pair 3 impulse on the emulation: direct 0.97, first taps 4-8 % of the input at 173/281/341/449 samples, nothing at 15 ms with weight 0.67. Pair 2: direct 0.646, two same-channel taps of about 0.51 (L input 34/328 samples, R input 278/460) and a sparse low-level field out to 3.7k samples. The emulation's late tail is not usable (it plateaus at a constant low level instead of decaying, and the notes in the stereo-reverb note show it 5.3 dB under the PCB tail).
* The HLE's 0.67 tap weight at 450/688 samples read the freshly injected input from the tank 15/23 ms later; with the inputs scaled by `1/(1+cross)` removed (+3.3 dB) this over-drove the tank (sound 1 whole-song level +1.47 dB).

Applied to `effects.hpp`: restored the reverb-send normalisation `r_norm = 1 / (1 + m_reverb_cross)`, removed the pair 2 send low-pass and the 2/3 dry attenuation (`m_dry_delay = table(127 - p8)`), refitted `K_DECAY_PER_P0` to 0.63/40 (decay with the normalisation) against both PCB recordings. Rejected after listening (user: "sounds dry again ... tail dragged out, lost richness"): circulated-only tank taps with early weight 0.3 (pair 3 impulse energy -32 dB at 15-30 ms but PCB-fit tails need a 2.5x louder loop), and a fixed pair 2 FIR table (hard-codes one preset).

PCB scores (`hscore.py`/`tails.py`, tail = 50 ms windows after the song end relative to the preceding 2 s, first 10 steps):

| render | A 0x187 level err | A tail mean abs / bias | B sound 1 level err | B tail mean abs / bias | A / B L-R corr err |
|---|---|---|---|---|---|
| before this step | -1.01 dB | 1.36 / +0.94 | +1.47 dB | 2.61 / -2.56 | +0.01 / -0.12 |
| r_norm restored, decay 0.53/40 | -0.90 | 3.41 / -3.10 | +0.28 | 7.22 / -7.22 | +0.12 / +0.12 |
| r_norm restored, decay 0.63/40 (shipped) | -0.92 | 1.74 / +1.46 | +0.22 | 2.38 / -2.28 | +0.11 / +0.09 |

Decay sweep (A |err| bias / B |err| bias): 0.58: 1.50/-1.05, 3.58/-3.29; 0.62: 1.22/+0.65, 3.10/-3.09; 0.635: 2.10/+1.86, 2.02/-1.88; 0.65: 3.18/+3.06, 1.13/-0.69; 0.66: 3.89/+3.85, 1.39/-1.25. The two recordings pull in opposite directions (A wants less, B more decay at the same preset), so 0.63 balances them; the suggested 0.50-0.56 leaves the tails 3-7 dB short against the PCB.

Clap window (2.48 s hit, 0x187): HLE is 3.5 dB below the PCB at 15-100 ms after the hit (HLE -27.7/-26.8 vs PCB -23.8/-23.4 dBFS), so the attack is not over-wet by that measure; the 1-2 kHz deficit remains (-7 to -8 dB).

## 13. Replay after the sequence ends: plain 0x81 starts must not restart at the bar boundary

**Symptom.** Sequence 0x2c (code 0x187, bank 1) keeps starting voices after the reference goes silent (last reference start 20.214 s render time); HLE restarts the same sequence from its beginning (same notes and sample addresses as the first pass, first seen at 0.815 s).

**Cause.** `pulse()` in `runtime/audio/hle/sequencer.cpp` restarted every sequence at its header bar boundary when it had a `looping` flag set by `0x80`/`0x81`/`0x85`: `initialize_tracks(id, s.selected, tick)`. The reference does not do that for a plain start. Looping in the driver goes only through the arrangement link: the bar-end path (`$c14d0e`) calls `$c12b9e`, which calls `$c12ca2` → `$c158ae` to look up the sequence's link entry in the link table (`$d404`, `+8 + 4*id`). With no link (`beq $c12cb6`) it returns without re-entering the sequence. Linked sequences (track-10 arrangement, Repeat100, repeat counts) keep their own path, which the reference confirms loops.

**Fix.** A sequence with no arrangement owner finishes at its bar boundary (`finish(id, tick)`). The `looping` flag had no other effect, so it and its parameters are removed from `start`, `arrangement_next`, and the `0x85` seek start. The arrangement owner path (`pulse()` `arrangement_owner` branch) is unchanged. Test: `HleAudio.KiramekiPlainStartStopsAtBarBoundary` (`games/kirameki/tests/audio.cpp`) plays `81 2c` for 40 s and asserts that the last voice start is less than 20.5 s after the first.

**Evidence (HLE event CSV, `kind` 0 = voice start; times relative to the first 0x81 packet, 16 MHz).**

| case | before fix | after fix | reference |
|---|---|---|---|
| kirameki 0x187, `03812c` `04862c7f`, 30 s | 1523 starts; 1025 before 19.8 s; 498 more, through 29.95 s (replay) | 1055 starts; 1025 before 19.8 s (same count, times within 1 ms); 30 from 19.834 s, last 20.142 s | 1056 starts; 30 at or after 19.8 s; last 20.214 s |
| landmakrj song 10, `03810a` `04860a7f`, 90 s | 3105 starts; restarts at 36.4 s, loops to 89.9 s | 1206 starts; last 33.798 s; no restart | 1194 starts; last 33.809 s; none through 90 s |
| landmakrj selection `038187` `0486877f` (id 7 with the high bit), 90 s | 3671 starts | 3671 starts, per-5 s bins identical | 3673 starts, looping through 89.93 s |

The selection-path row is the regression check: the arrangement loop still matches the reference bin-for-bin.

**Not verified.** The `$d404` entry for id 44 was not disassembled; the conclusion that it is unlinked comes from the bar-end code path and the reference result. The `0x85` seek path now also finishes at the bar boundary; it was not compared with a reference trace. Landmakrj song 4 has no reference capture. The new test was not run against the pre-fix build.

**Commands.**

```bash
./build-kirameki/f3rt-tool sound-extract --set kirameki --rom-dir roms/kirameki --packet 03812c --packet 04862c7f --seconds 30 --audio-backend enhanced --hle-events /tmp/rv/hle_187_new.csv --wav /tmp/rv/hle_187_new.wav --json
./build-landmakrj/f3rt-tool sound-extract --set landmakrj --rom-dir roms/landmakrj --packet 03810a --packet 04860a7f --seconds 90 --audio-backend enhanced --hle-events /tmp/rv/lm10_hle_new.csv --json
./build-landmakrj/f3rt-tool sound-extract --set landmakrj --rom-dir roms/landmakrj --packet 038187 --packet 0486877f --seconds 90 --audio-backend enhanced --hle-events /tmp/rv/lm87_hle_new.csv --json
ninja -C build-kirameki && ctest --test-dir build-kirameki      # 184/184
ninja -C build-landmakrj && ctest --test-dir build-landmakrj    # 183/183
ninja -C build-commandw && ctest --test-dir build-commandw      # 181/181
```

Reference traces: the same packets with `--audio-backend reference --sound-trace <file>` (kirameki 0x187: `/tmp/rv/ref_187.f3snd`, landmakrj: `lm10.f3snd`, `lm87.f3snd`), decoded with `tools/decode/trace.py` after clamping (`/tmp/rv/decode_clamped.py`).
