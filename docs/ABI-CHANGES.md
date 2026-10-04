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

Native ABI memory callbacks synchronize devices to `cpu.cycles` before sound
mailbox reads/writes and sound reset-line accesses, including wide accesses
starting outside but overlapping the mapped range. `f3_reset_devices` also
drains preceding device time before changing reset state. Otherwise a native
block can expose a new command/reset to sound execution in the block's past.
This does not charge extra CPU cycles, materialize lazy flags on memory
callbacks, or deliver main IRQs inside an instruction. IRQ entry remains at
the next dispatch boundary. ABI layout/version 2 and multi-instruction blocks
are unchanged; direct interpreter memory callbacks retain their existing
instruction-boundary synchronization and do not re-enter the sound core.

TRAP #n vectors 32–47 have a full EC020/68020 charge of 24 cycles. The semantic
reference retains the four-cycle opcode charge in addition to the 20-cycle
exception-table entry; `f3_exception` owns all 24 cycles for generated code.
Other exception classes and CPU variants are unchanged.

Sound 68000 timing is independently calibrated against executed reference
microprograms, not inferred from the main EC020 table. ADDQ.W-to-An, long
register arithmetic, immediate operand widths, TAS effective addresses and
register bit-index timing have dedicated regressions. Other CPU models retain
their prior costs. This changes no ABI fields or main-CPU generated costs and
does not establish integrated audio parity.

The sound 68000 charges the nominal no-wait 44 cycles for IRQ entry regardless
of whether IACK selects an autovector or a device-supplied vector. This assumes
the manual's four-clock IACK, not arbitrary bus wait states. The vector still
selects the handler and exception frame. For a nonzero divisor, DIVU.W follows the
measured operand-dependent cost: 10 cycles on quotient overflow, otherwise
76–136 cycles, plus the effective-address cost. Results, flags, divide-by-zero,
MUL, DIVS, Group 2 exceptions and all other CPU-model timings are unchanged.
These corrections neither change ABI 2 nor establish audio parity.

The sound 68000 MULS.W timing includes the final 1-to-0 Booth transition for
strictly positive word sources. The former shift-until-zero loop omitted it,
undercharging these operands by two cycles. Register, memory and immediate
forms now match executed reference microprograms. Zero/negative sources,
MULU, products/flags and all other CPU-model timings retain their prior behavior.

When a 68000 STOP lowers SR and exposes an already-pending IRQ, its idle-budget
clamp must not discard interrupt-entry cycles. This transition charges four
instruction clocks, one four-clock STOP polling iteration, and 44 IRQ-entry
clocks: 52 total before the handler. The polling iteration matches the executed
reference microprogram and its latch-before-mask-update microcode. Normal STOP
waiting, later IRQ wakeups and other CPU models are unchanged. This is a scoped
reference timing correction, not a general bus-cycle-accurate execution model.

Sound scheduling is independent of native main-block partitioning. `Audio`
advances to the next sample or sound-CPU dispatch deadline, applies device
edges first, then dispatches one sound instruction and retains its full cycle
debt. Fractional clocks survive all caller chunks. Main native blocks and ABI 2
deadlines remain unchanged; there is no public header or callback change.
Sound instructions and their bus effects are still atomic, not microcycle
accurate. Equal native/interpreter PCM does not imply MAME waveform parity.

The DUART now models both transmitters' ready/empty transitions, one-byte
holding registers, framed serial clocks, enable/reset commands and TX-ready
interrupts. F3's external clocks are 1 MHz (A) and 500 kHz (B); the game's
channel-B 8N2 configuration divides the latter by 16. Counter-derived TX clocks
also retain the required divide-by-16 prescaler. TX pins are unconnected, so no
serial payload storage or output callback is needed. This corrects the ROM's
TXEMPTY polling duration without modifying CPU instruction costs or ABI 2.

DUART counter deadlines are stored in crystal clocks, independent of subsequent
clock-source/preset writes. Restart discards prior divider phase. Board reset
preserves an already queued expiration and the timer output phase, reproducing
the observed reference reset path; register reset still clears IMR/ISR/IVR.
This intentionally follows MAME's scheduled-event behavior rather than its
contradictory reset comment, and is not asserted to model physical RESET.
No CPU ABI or generated-code contract changes are involved.

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

## Optional game-data video interface

The C CPU ABI remains version 2. Japan's existing configured-hook mechanism
calls `f3_landmakr_video_hook(f3_cpu *)` before selected display-producer
instructions with materialized flags. The hook observes registers and
ROM/work-RAM data; it does not skip instructions or change CPU state.

The runtime C++ `Machine` owns an optional `GameVideo`; VBSTART selects it
instead of the default FDP renderer when requested. Graphics/control writes
notify it of only producer PC and destination address, to reject missing
producers rather than copy FDP values. `Video` exposes immutable decoded ROM
assets and optional diagnostic layer/row readback. The oracle renderer and
its one-frame sprite latch remain intact. These are C++ runtime interfaces,
not changes to generated register layouts, bus callbacks or CPU timing.
