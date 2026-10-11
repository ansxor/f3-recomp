# MAME Land Maker Capture & Verification Tooling

This directory provides tooling for capturing baseline attract-mode frames, video/audio RAM state, write-only scroll registers, and audio from MAME to verify against the `f3rt` standalone runtime and renderer.

---

## 1. Directory Capture Protocol

Captures are stored in an organized per-frame directory layout:

```text
captures/landmakrj_attract/
├── metadata.json              # Global capture metadata (dimensions, visarea, frame list)
├── attract.wav                # Emulated sound output captured via MAME -wavwrite
├── frame_0300/
│   ├── metadata.json          # Frame metadata (frame number, dimensions, register hex)
│   ├── control.bin            # 0x20 bytes (BE): Write-only scroll registers (m_control_0, m_control_1)
│   ├── palette.bin            # 0x8000 bytes (BE): Palette RAM (0x440000..0x447fff, 8192 x 32-bit colors)
│   ├── graphics.bin           # 0x40000 bytes (BE): Graphics RAM (0x600000..0x63ffff: spriteram, pf_ram, textram, charram, lineram, pivot)
│   ├── mainram.bin            # 0x20000 bytes (BE): Main RAM (0x400000..0x41ffff)
│   ├── spriteram_active.bin   # 0x10000 bytes (BE): Buffered spriteram displayed on screen for this frame
│   ├── reference.argb         # 320x232x4 raw pixel bytes (BGRA little-endian as output by MAME)
│   └── reference.bmp          # 320x232 32-bit BMP image for visual inspection
└── frame_0301/ ...
```

### Video timing and capture phase

Protocol version 2 pairs state sampled on frame callback N with `screen:pixels()` sampled on callback N+1. MAME swaps its bitmap before the Lua frame-done callback; the pixel API therefore exposes the previous completed frame. Version-1 captures incorrectly paired current RAM with previous pixels. Re-capture them for parity work.

Land Maker's baseline MAME renderer uses one-frame sprite buffering: current palette/playfield/text/line RAM is combined with sprites built from frame N-1. `spriteram_active.bin` preserves that prior sprite RAM separately. Control-register taps are retained strongly and reinstalled on soft reset.

Observed verification: 25 phase-correct captures, frames 600–3480 at step 120, replay with **zero RGB mismatches across all 1,856,000 pixels**. An additional 34 consecutive animated frames established the one-callback pixel delay. This validates comparison against captured MAME output, not native game execution or physical-chip behavior. Discrepancies and superseded observations are archived in [historical development notes](../../docs/developer/DECISIONS.md).

---

## 2. Prerequisites & MAME Binary Acquisition

Use the preserved executable `/Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3`; do not rebuild or replace it.

- Clean source revision: `cfc4760a3be9c5a79846b19b6a573cb38459fa7e`
- Binary SHA-256: `156ffb34020580bc05d9ae0854b72e0f7f7168b1b1f466127d5d0c2f3610b91e`
- Expected warnings: the undumped `palce16v8q-d77-15.ic21`, and the reduced-subtarget `spcinvdj` missing-parent diagnostic. Neither warning prevented the exercised Land Maker captures.

---

## 3. ROM Staging (`stage_roms.py`)

The provided Land Maker ROMs correspond to the Japanese set (`landmakrj`, Ver 2.01J 1998/06/01). Sound ROMs `e61-14.32` and `e61-15.33` are 128KB (0x20000 bytes); appending 0x20000 bytes of `0xFF` padding yields 256KB ROMs whose CRCs match MAME (`18961bbb` and `2c64557a`).

To verify and stage into an untracked zip for MAME:
```bash
uv run python tools/mame/stage_roms.py
```
Options:
- `--check-only`: Verifies CRCs without writing files.
- `--out-zip <path>`: Specifies custom output ZIP path (default: `tools/mame/staged_roms/landmakrj.zip`).
- `--source <dir>`: Japanese game chip directory.
- `--board-source <dir>`: Common F3 board PLDs; defaults to the supplied Puchi Car set. Four PLDs are selected by verified CRC.

---

## 4. Running MAME Capture (`run_capture.sh`)

To execute MAME and record attract-mode frames:
```bash
./tools/mame/run_capture.sh
```

Options:
- `--mame <path>`: Path to MAME/f3 executable (default: `/Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3`)
- `--outdir <dir>`: Destination directory (default: `captures/landmakrj_attract`)
- `--start-frame <N>`: First state frame to capture (default: 300; cold-boot self-test can still be visible).
- `--count <N>`: Number of frames to capture (default: 10)
- `--step <N>`: Cadence between captured frames (default: 1)
- `--wav <path>`: Output audio WAV file (default: `<outdir>/attract.wav`)
- `--throttle`: Run throttled instead of maximum emulation speed
- `--dry-run`: Display command line without running

---

## 5. Comparing Frames (`f3 compare frames`)

`uv run f3 compare frames` compares reference MAME frames against `f3rt` rendered frames.

### Single Frame Comparison
```bash
uv run f3 compare frames captures/landmakrj_attract/frame_0300/reference.bmp f3rt_frame_0300.bmp --diff diff_0300.bmp
```

### Directory Comparison
```bash
uv run f3 compare frames captures/landmakrj_attract/ f3rt_output_dir/
```

### Quantitative Metrics Produced
- **Resolution**: 320x232 (74,240 pixels per frame)
- **Exact Matches**: Pixel count and percentage
- **Tolerance Matches**: Matches within specified channel tolerance (`--tolerance <int>`)
- **Max Error**: Maximum absolute difference across R, G, B channels
- **MAE / RMSE**: Mean Absolute Error and Root Mean Squared Error
- **PSNR**: Peak Signal-to-Noise Ratio in dB
- **Error Distribution**: Binned counts (0, 1-2, 3-7, 8-15, 16-31, 32+)
- **Diff Image**: BMP highlighting differences with adjustable amplification (`--diff-amp <int>`)
- **JSON Output**: For automated CI testing via `--json`

## 6. Audio device replay

Capture sound-ROM writes independently of main-CPU scheduling. Use fresh
NVRAM/config directories so the reference starts from the same cold boot:

```sh
F3_AUDIO_TRACE=build/mame-audio.trace \
  /Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3 landmakrj \
  -rompath tools/mame/staged_roms \
  -autoboot_script tools/mame/audio_trace.lua -autoboot_delay 0 \
  -wavwrite build/mame-audio.wav -video none -nothrottle \
  -noautoframeskip -frameskip 0 -skip_gameinfo \
  -nvram_directory build/mame-nvram-audio-trace \
  -cfg_directory build/mame-cfg-audio-trace -seconds_to_run 62

build/runtime/f3rt-replay --rom-dir /path/to/roms/landmakr \
  --audio-trace build/mame-audio.trace --output build/replay-audio.wav
```

The binary trace begins with eight bytes `F3AUD2\0\0`. Each 16-byte,
little-endian record contains a 64-bit timestamp in 16 MHz ticks, a 32-bit
sound-bus address, 16-bit data, and a 16-bit byte-lane mask. An address of
`0xfffffffe` resets the sound-board devices; `0xffffffff` terminates the stream
at the recorded emulated time. Reset notifications record the event and reinstall
the retained write tap. Version 1 traces omitted board resets and must be
recaptured. Traces contain game-derived data and must remain under ignored `build/`.

Replay drives the real standalone OTIS/ESP/volume devices without executing
the main or sound CPU. It isolates audio-device behavior; it does **not**
establish native game acceptance. Baseline WAV output is 48 kHz; replay is at
the advertised OTIS integer stream rate (29761 Hz with 32 voices). Compare
after accounting for output resampling and latency, not by WAV byte identity.
Native-game audio must be checked separately against the baseline.

The default gain model reproduces observed MAME routing. Embedders may select
`Audio::GainModel::SingleStage` explicitly to investigate analog attenuation;
that alternative is not claimed to match the physical board.

### Reproducible waveform metrics

`f3 compare audio` requires NumPy and SciPy in the analysis Python
environment. It uses one fixed latency for the entire comparison interval,
does not fit gain or warp time, and reports per-channel RMS error, correlation,
SNR, and two-second window metrics. A negative lag means the runtime is early.
Metrics are not an automatic parity verdict.

```sh
uv run --group analysis f3 compare audio \
  build/mame-audio.wav build/replay-audio.wav \
  --start 18 --end 54 --json build/audio-comparison.json
```

For device-math comparisons, capture a separate fresh baseline with
`-samplerate 29761`, then replay its trace. This avoids conflating MAME's host
resampler with SciPy's band-limited resampling. Native-rate device replay over
18–54 seconds measured correlation 0.999978 on both channels, about 1.3 PCM LSB
RMS error, and identical channel peaks (1239/1264), after one sample of latency
alignment. The actual native-game comparison remains separate; before EEPROM
busy timing, its audio and video were approximately seven frames early.
