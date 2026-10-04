# CPU ABI changes

## Version 2 — instruction-granular scheduling

`f3_cpu.dispatch_deadline` is a 64-bit main-cycle threshold immediately after
`cycles`. Generated code and the runtime must both use ABI 2; rebuild all
consumers. Between instructions, a native block must publish its successor PC,
materialize SR and return when `cycles >= dispatch_deadline`. This preserves
multi-instruction blocks and lazy flags without delaying hardware events until
the end of a block.

Zero forces a boundary recheck. A runnable `f3_boundary` publishes the minimum
of the next vblank, delayed IRQ3 and watchdog expiry. Redirects, STOP and device
resets leave the cache invalid. STOP advances to that same earliest event,
including watchdog expiry. Lowering the SR interrupt mask invalidates the
deadline in both `f3_set_sr` and interpreter state export, so an already-pending
IRQ is reconsidered immediately. Raising the mask leaves a valid deadline
intact. A watchdog strobe may retain an earlier conservative threshold but must
not overwrite an outstanding zero/recheck.

TRAP #n vectors 32–47 have a full EC020/68020 charge of 24 cycles. The semantic
reference retains the four-cycle opcode charge in addition to the 20-cycle
exception-table entry; `f3_exception` owns all 24 cycles for generated code.
Other exception classes and CPU variants are unchanged.

## Version 1 — 2026-10-03

`include/f3rt/cpu_abi.h` is the frozen initial C interface. Generated blocks use numeric 68020 registers, materialized SR at non-memory callbacks, cumulative cycle accounting, big-endian bus accessors, a per-block boundary hook, exception entry, sorted dynamic block registration, device reset, and a one-instruction interpreter fallback. `f3_set_sr` handles user/interrupt/master stack switching and invalidates lazy flags. Blocks set successor PC and return; `f3_dispatch` owns iterative lookup and fallback. The runtime pointer is opaque and runtime-owned. No serialized struct layout is promised across pointer widths.

Main CPU clock: 16 MHz, matching MAME. The boundary advances hardware using the delta in `cpu.cycles`; generated cycle estimates affect scheduling accuracy and must not be advertised as instruction-cycle exact.

The first publication and peer draft crossed in transit. The final agreed v1 retains `F3RT_ABI_VERSION`, `f3_block.address`, and runtime-owned `f3_dispatch`, and adds the peer's MSP/control registers, lazy-flag storage, `halted`, and `f3_reset_devices`. There is no exported lookup function.

Group-2 exceptions (vectors 5, 6, 7, 9) use a 68020 format-2 stack frame: `return_pc` is the stacked resume PC, and `cpu->pc` on entry is the instruction address. Other normal exceptions use format 0; a master-mode interrupt also produces the format-1 throwaway frame on ISP. Trace-active execution routes through the interpreter, with Musashi trace support enabled.

`f3_exception` owns the full exception cycle charge. Generated trap, privilege, divide-by-zero, and other exception paths must not add the instruction's normal base/nominal charge. Current costs: bus/address error 50; illegal/A-line/F-line/TRAPV 20; TRAP #n 24 (corrected in ABI 2); divide-by-zero 38; CHK 40; privilege 34; trace 25; format error 4; uninitialized/spurious/autovectored interrupt 30; remaining vectors 4. These are emulator-model timings, not measured hardware bus-cycle accuracy or a blanket timing-parity claim.

Initial and watchdog main-CPU resets consume the pinned 68EC020 reset latency
before the first generated block or fallback instruction. The runtime drains
the reference core's four reset cycles immediately and charges `cpu.cycles`;
importing canonical state must not silently lose that debt. This is distinct
from the privileged guest `RESET` instruction, which resets external devices.

Sound-CPU reset writes at `$c80000`/`$c80100` retain DUART, DSP, volume and
sound RAM state. Whole-machine/watchdog reset instead uses the C++ board-level
`Audio::reset_board`: reset DUART/DSP/volume, reload the first eight sound-ROM
bytes into work RAM, and hold the CPU. OTIS voices, remaining work RAM, queued
samples and the fractional output clock are preserved. The C CPU ABI is unchanged.

Raster time zero is the reference screen's VBSTART beam epoch. IRQ2/render
events occur at `ceil(n * 432 * 262 * 16000000 / 6671500)` main ticks for
`n = 1, 2, ...`; IRQ3 follows each IRQ2 by 10000 ticks. Watchdog reset does not
restart the continuous raster clock.

The pinned 68EC020/68020 MOVEM reference charges three cycles per stored
register for either width, and four per loaded register, in addition to the
opcode/addressing cost. Generated blocks and the semantic reference must use
the same distinction. Sound 68000 and other CPU-model costs are unchanged.

EC020/68020 ROL, ROR, ROXL and ROXR have no count-dependent cycle surcharge
for either immediate or register counts. The pinned reference stores shift
timing as cycles per count, not a shift exponent; zero must mean zero extra
cycles. Operand, flag and other CPU-model behavior is unchanged.
