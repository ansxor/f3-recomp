# CPU ABI changes

## Version 1 — 2026-10-03

`include/f3rt/cpu_abi.h` is the frozen initial C interface. Generated blocks use numeric 68020 registers, materialized SR at non-memory callbacks, cumulative cycle accounting, big-endian bus accessors, a per-block boundary hook, exception entry, sorted dynamic block registration, device reset, and a one-instruction interpreter fallback. `f3_set_sr` handles user/interrupt/master stack switching and invalidates lazy flags. Blocks set successor PC and return; `f3_dispatch` owns iterative lookup and fallback. The runtime pointer is opaque and runtime-owned. No serialized struct layout is promised across pointer widths.

Main CPU clock: 16 MHz, matching MAME. The boundary advances hardware using the delta in `cpu.cycles`; generated cycle estimates affect scheduling accuracy and must not be advertised as instruction-cycle exact.

The first publication and peer draft crossed in transit. The final agreed v1 retains `F3RT_ABI_VERSION`, `f3_block.address`, and runtime-owned `f3_dispatch`, and adds the peer's MSP/control registers, lazy-flag storage, `halted`, and `f3_reset_devices`. There is no exported lookup function.

Group-2 exceptions (vectors 5, 6, 7, 9) use a 68020 format-2 stack frame: `return_pc` is the stacked resume PC, and `cpu->pc` on entry is the instruction address. Other normal exceptions use format 0; a master-mode interrupt also produces the format-1 throwaway frame on ISP. Trace-active execution routes through the interpreter, with Musashi trace support enabled.
