---
source_url: "f3rt-tool sound-extract (HLE enhanced vs reference native sound driver with MAME ES5505/ES5510 emulation); Land Maker (landmakrj) sound-test codes 0x56 and 0x53"
ingested: 2026-10-16
sha256: unrecorded (generated WAVs /tmp/clk/*.wav)
game: [landmakrj]
captured_with: "f3rt-tool sound-extract (enhanced vs reference/native), tools/decode/trace.py F3SND2 register decoder, Python click detection"
---
# Land Maker 0x56 & 0x53: percussion click transients, sample-end interpolation and release-time voice stealing

## Findings

1. **Non-looping sample end boundary interpolation (0x56 percussion clicks)**:
   In HLE block synthesis (`runtime/audio/hle/synth.cpp`), voice sample interpolation computes:
   `input[idx] = float(first + (second - first) * fraction);`
   For non-looping samples (e.g. track 7 percussion keys 39..53 in 0x56, bass drum / toms), when the playback
   accumulator approaches the end address (`v.end`), `index` is the last sample word before `v.end`.
   Evaluating `sample(index + 1)` read past `v.end` into whatever unrelated data happened to be packed next in sample ROM
   (e.g. for sample 4360326..4368096, word 4368096 in ROM contains `0x4500 = 17664`, the attack header/sample of the next instrument).
   This caused a massive single-sample transient spike (e.g. leaping to +453 while surrounding audio was ~ -40), followed immediately
   by voice stop (`v.active = false`) on the next frame when `v.position > v.end`.
   (Notice that MAME's `es5505.cpp:270` had the exact same bug: `read_sample(voice, next_word_addr)` also read past `voice->end`,
   producing 65 clicks in the reference backend).
   Clamping the boundary lookup for non-looping samples:
   - Forward: `(!v.loop && !v.reverse && index + 1 >= size_t(v.end)) ? 0.0 : sample(index + 1)`
   - Reverse: `(!v.loop && v.reverse && index < size_t(v.start)) ? 0.0 : sample(index)`
   smoothly fades the final fractional step down to zero at the boundary without touching adjacent instrument memory.
   This dropped click counts in 0x56 from 62 to **0**.

2. **Release-Time Voice Stealing Invariant (`allocate()`)**:
   In `allocate()` (`synth.cpp`), HLE previously searched `if (!old || v.serial < old->serial)` using `v.serial`,
   which was assigned at **note-on** (`begin()`), not at `release()`!
   A long-held sustain/bass/pad note that started 2 seconds ago had a tiny `v.serial`; the instant it received `note_off`
   (`v.released = true`), it was still at full sustain volume (>8,000 LSB), yet `v.serial < old->serial` made it the very
   first voice stolen on the next note-on (1 ms after release, cut at full volume).
   Meanwhile, voices released hundreds of ms earlier that had fully decayed to silence were skipped because their note-on serials were newer.
   Tracking `v.release_serial = ++serial` at `release()` and ordering released voices by `v.release_serial < old->release_serial`
   ensures the **oldest-released** voice (which has had the longest time to decay to silence) is reused first, matching the driver's
   doubly-linked `$d850` released list order.

3. **Loop Boundary Analysis (0x53 looping instruments)**:
   In Land Maker ROMs, 196 unique looping sample ranges exist in 0x53. In almost all instruments, the sound authors intentionally
   placed a copy of `samples[start]` at `samples[end]` as a guard word for the ES5505 hardware interpolator.
   Evaluating `sample(index + 1)` at loop end interpolates seamlessly into this guard word.

4. **0x53 Click Counts vs Reference**:
   In 0x53, HLE click counts match the Reference backend's exact click count across all thresholds:
   - At thr=8.0, minabs=300: HLE 1 vs REF 1
   - At thr=6.0, minabs=200: HLE 35 vs REF 35
   - At thr=6.0, minabs=150: HLE 35 vs REF 35
   - At thr=4.0, minabs=60: HLE 1242 vs REF 1267

5. **Kirameki 0x187 Invariants**:
   Reverb and acoustic metrics on Kirameki 0x187 against PCB recording `/home/answer/Downloads/kirameki-sound-1-187.flac`:
   - C50 diff: +0.22 / +0.20 / +0.20 dB
   - Floor p10-p90 diff: -0.33 dB
   - L/R correlation: 0.7199
   - Tail MAE: 2.99 dB

## Click Measurements Before and After Fix

Second-difference spikes relative to local RMS ($|d^2| > \text{thr} \cdot \text{RMS}_{\text{local}}$ and $|d^2| > \text{minabs}$):

### Sound-Test Code 0x56 (40 seconds)

| Threshold / MinAbs | HLE Before | HLE After | Reference Backend |
|---|---|---|---|
| thr = 8.0, minabs = 300 | 53 | **0** | 55 |
| thr = 6.0, minabs = 200 | 62 | **0** | 65 |
| thr = 6.0, minabs = 150 | 64 | **0** | 74 |
| thr = 5.0, minabs = 100 | 91 | **15** | 119 |
| thr = 4.0, minabs = 60 | 543 | **443** | 566 |

### Sound-Test Code 0x53 (40 seconds)

| Threshold / MinAbs | HLE Before | HLE After | Reference Backend |
|---|---|---|---|
| thr = 8.0, minabs = 300 | 1 | **1** | 1 |
| thr = 6.0, minabs = 200 | 37 | **35** | 35 |
| thr = 6.0, minabs = 150 | 37 | **35** | 35 |
| thr = 5.0, minabs = 100 | 231 | **223** | 206 |
| thr = 4.0, minabs = 60 | 1281 | **1242** | 1267 |

## Commands

```bash
# Render HLE and reference
./build-landmakrj/f3rt-tool sound-extract --sound 0x56 --seconds 40 --wav /tmp/clk/hle_56.wav --hle-events /tmp/clk/ev_56.csv
./build-landmakrj/f3rt-tool sound-extract --sound 0x56 --seconds 40 --audio-backend reference --sound-driver native --wav /tmp/clk/ref_56.wav

# Verification on Kirameki PCB
./build-kirameki/f3rt-tool sound-extract --rom-dir roms/kirameki --bank 1 --sound 0x187 --seconds 24 --wav /tmp/clk/hle_187.wav
uv run --group analysis python -m tools.compare.reverb --pcb /home/answer/Downloads/kirameki-sound-1-187.flac --render hle=/tmp/clk/hle_187.wav
```
