# CPU ABI changes

## Version 1 — 2026-10-03

`include/f3rt/cpu_abi.h` is the frozen initial C interface. Generated blocks use numeric 68020 registers, eager SR flags, cumulative cycle accounting, big-endian bus accessors, a per-block boundary hook, format-0 exception entry, sorted dynamic block registration, and a one-instruction interpreter fallback. `f3_set_sr` handles supervisor/user stack switching. Blocks set successor PC and return; dispatch is iterative. The runtime pointer is opaque and runtime-owned. No serialized struct layout is promised across pointer widths.

Main CPU clock: 16 MHz, matching MAME. The boundary advances hardware using the delta in `cpu.cycles`; generated cycle estimates affect scheduling accuracy and must not be advertised as instruction-cycle exact.
