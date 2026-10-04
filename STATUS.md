BLOCKED: GPU parity/native invariants and 60 Hz headroom are verified; opt-in interpolation exploration remains.

# Phase 5 — GPU video

CPU row threading and the non-interpolated SDL3 GPU port are complete.
Metal: 32 seeded scale/border runs, 128,000 native frames, 4320 GPU/CPU
full-frame comparisons and 4096 comparisons per isolated layer, zero mismatches.
Native output retains 25/25 MAME frames, RAM600, WAV and zero CPU fallback.
Independent CPU-backend snapshots/presentation and allocation-free save/restore
are verified. Ending coverage is an induced producer boundary, not a played ending.

Scale 4/border 48 GPU render budget: mean/p95/worst 10.443/11.082/11.720 ms,
including native CPU emulation and fenced compositor readback. Actual unthrottled
SDL GPU window: 3600 frames in 31.56 seconds, 114.1 fps including startup.
Detailed timing scope and full CPU/GPU scale 1/2/4 tables: docs/GPU-VIDEO.md.

Next checkpoint is separate: characterize the actual character-select water,
compare safe linear/fitted interpolation, and prove region/garbage guards.

Required distinct local checkpoints: `gpuvideo-1-threaded-cpu`,
`gpuvideo-2-gpu-parity`, `gpuvideo-3-interp`. No pushes or committed ROMs/binaries.
Design and exact scale/sprite contracts: [docs/GPU-VIDEO.md](docs/GPU-VIDEO.md).
