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

### Video Timing & Buffering Phase Evidence
In Taito F3 (`taito_f3_v.cpp`), Land Maker uses `sprite_lag = 1`:
- At each vertical frame update, `scanline_draw()` executes first, rendering playfields, text, pivot, and palette from current RAM, and sprites from `m_sprite_framebuffer` (which was rendered at the end of frame $N-1$).
- Then `get_sprite_info()` and `draw_sprites()` execute, converting current `spriteram` (0x600000..0x60ffff) into `m_sprite_framebuffer` for frame $N+1$.
- `emu.register_frame_done` fires immediately after this drawing pipeline finishes.
- Therefore:
  * `reference.argb` and `reference.bmp` represent the output of `scanline_draw()`.
  * `palette.bin`, `control.bin`, and `graphics.bin` represent the RAM state used for the background layers.
  * `spriteram_active.bin` holds the exact spriteram from frame $N-1$ that was actually rendered into pixels.

---

## 2. Prerequisites & MAME Binary Acquisition

Per project orchestration instructions:
- Do **not** compile a full MAME build here.
- The baseline MAME binary is built with `SUBTARGET=f3` in one of the bug worktrees (e.g. `tcobra2-pan-120` or `recalh-service3-8723`).
- Once confirmed clean (`git log origin/master..HEAD` is empty), copy the resulting `f3` executable:
  ```bash
  mkdir -p /Users/darien/Workspace/f3-stuff/tools/mame-baseline/
  cp <worktree-path>/f3 /Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3
  chmod +x /Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3
  ```

---

## 3. ROM Staging (`stage_roms.py`)

The provided Land Maker ROMs correspond to the Japanese set (`landmakrj`, Ver 2.01J 1998/06/01). Sound ROMs `e61-14.32` and `e61-15.33` are 128KB (0x20000 bytes); appending 0x20000 bytes of `0xFF` padding yields 256KB ROMs whose CRCs match MAME (`18961bbb` and `2c64557a`).

To verify and stage into an untracked zip for MAME:
```bash
python3 tools/mame/stage_roms.py
```
Options:
- `--check-only`: Verifies CRCs without writing files.
- `--out-zip <path>`: Specifies custom output ZIP path (default: `tools/mame/staged_roms/landmakrj.zip`).

---

## 4. Running MAME Capture (`run_capture.sh`)

To execute MAME and record attract-mode frames:
```bash
./tools/mame/run_capture.sh
```

Options:
- `--mame <path>`: Path to MAME/f3 executable (default: `/Users/darien/Workspace/f3-stuff/tools/mame-baseline/f3`)
- `--outdir <dir>`: Destination directory (default: `captures/landmakrj_attract`)
- `--start-frame <N>`: First frame to capture (default: 300, after boot self-test into attract)
- `--count <N>`: Number of frames to capture (default: 10)
- `--step <N>`: Cadence between captured frames (default: 1)
- `--wav <path>`: Output audio WAV file (default: `<outdir>/attract.wav`)
- `--throttle`: Run throttled instead of maximum emulation speed
- `--dry-run`: Display command line without running

---

## 5. Comparing Frames (`tools/compare_frames.py`)

`tools/compare_frames.py` is a standalone Python tool (zero external dependencies) that compares reference MAME frames against `f3rt` rendered frames.

### Single Frame Comparison
```bash
python3 tools/compare_frames.py captures/landmakrj_attract/frame_0300/reference.bmp f3rt_frame_0300.bmp --diff diff_0300.bmp
```

### Directory Comparison
```bash
python3 tools/compare_frames.py captures/landmakrj_attract/ f3rt_output_dir/ --diff-dir diffs/
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
