BLOCKED: GPU parity, performance, and opt-in interpolation acceptance are not yet verified.

# Phase 5 — GPU video

Implementation in progress. CPU compositor/reference and presentation-only
constraints remain mandatory; no completion claim before the seeded GPU parity,
60 Hz performance, fallback presentation, and interpolation boundary gates.

Required distinct local checkpoints: `gpuvideo-1-threaded-cpu`,
`gpuvideo-2-gpu-parity`, `gpuvideo-3-interp`. No pushes or committed ROMs/binaries.
Design and exact scale/sprite contracts: [docs/GPU-VIDEO.md](docs/GPU-VIDEO.md).
