# CPU ABI changes

## Version 3 — explicit ROM instruction-start exclusions

`f3_excluded_range` and `f3_register_exclusions` register immutable, sorted,
nonoverlapping `[start,end)` ROM intervals with reason/evidence strings.
Registration rejects overlap with native entries. Excluded PCs fail with their
address/range before interpretation, including odd PCs, 24-bit bus aliases and
diagnostic fallback.
ROM reads from these intervals remain legal.

The `f3_cpu` layout, instruction timing, lazy flags and scheduling contract are
unchanged. `Machine::excluded_code` and sound exclusion spans are immutable build
metadata, not snapshot state; canonical save/restore records remain unchanged.
Main and sound generation now require ABI 3. Regenerate both programs.

`Machine::use_native_sound` now takes the generated exclusion span explicitly.
Ordinary/full-tier sound tables contain exactly the even-address exclusion
complement. Dispatch subtracts preceding excluded words from the dense index
without allocating a second map. Explicit profile-slim tables are validated
sorted subsets of that complement and use sparse lookup. In every mode an
excluded physical target (including odd/upper-byte aliases) fails before any
opcode read, interpreter execution or profile-cold miss. Profile partitioning
and instrumentation leave ABI 3 and canonical state unchanged.

Both generators partition only the exclusion-filtered entries. Contradictory
profile hits/misses reject generation with CPU/address/range/reason; profiles
retain full-image CRC/base/size identities. Shared exception bodies are tiered
by their retained hot aliases.

Evidence, scanner/profile commands and per-region size experiment:
[BINSIZE-EXCLUDE.md](BINSIZE-EXCLUDE.md).

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

`GameVideoOptions` fixes presentation scale/border at construction.
`GameVideo::presentation()` returns a read-only span of that frame's pixels;
its contents change at the next render/reset. Default dimensions are 320×232;
optional dimensions are `(320 + 2*border)*scale` by `232*scale`.
`Machine::pixels` always remains the native 320×232 image, including when
presentation enhancements are enabled. This avoids changing capture, CRC or
CPU/device interfaces to accommodate optional display resolution.

## Machine snapshot contract and canonical state inventory

The C CPU ABI remains version 2. The C++ `Machine` adds state snapshot methods:
- `Machine::state_size() const -> size_t`
- `Machine::save_state(std::span<uint8_t> dst) const`
- `Machine::load_state(std::span<const uint8_t> src)`
- `Machine::state_crc() const -> uint32_t`

Snapshot storage has a fixed size for a configured machine, allocated once by
the rollback core (preallocated >= 16 slot ring). `save_state` and `load_state`
are allocation-free per frame. `state_crc()` computes the standard `f3rt::crc32`
over the canonical bytes written by `save_state()`. Its scratch buffer allocates
on the first call after configuration; rollback hashes its existing snapshots
instead. Snapshots are same-build, host-endian in-process state, not a portable
save-file format. Load only snapshots produced by the same machine configuration,
at frame boundaries on the emulation thread; no device reconfiguration or SDL
consumer may race save/load.

### Canonical state byte rules
1. No host pointers or virtual dispatch tables are written.
2. No uninitialized memory or compiler padding bytes exist in serialized records;
   all structures use 1-byte packed layouts.
3. Diagnostic counters (`native_blocks`, `fallback_instructions`, `fallback_hits`,
   `sound_trace`, video fallback counters) are excluded.
4. Native CPU lazy flags (`cc_src`, `cc_dst`, `cc_result`, `cc_op`, `cc_width`,
   `cc_mask`) and `dispatch_deadline` are serialized directly, preserving
   instruction-boundary lazy flags and deadline without state mutation.
5. Sound contexts support both static native recompiled execution (`SoundNative`)
   and interpreted Musashi 68000 execution (`Interpreter`). Musashi host callbacks
   and cycle tables are excluded from canonical serialization and preserved across
   process instances upon load.
6. Audio queued PCM is canonicalized: active frames from the circular buffer are
   serialized in sequential playback order, with remainder zeroed up to capacity.
   All hardware device states (ES5505 OTIS voices, ES5510 DSP DRAM and pipeline,
   MC68681 DUART timers/transmitters, MB87078 electronic volume) and mixer phase
   accumulators are completely inventoried.
7. Retained video state (TC0630FDP buffered spriteram, sprite framebuffer, spritelist,
   and optional GameVideo scene records) is restored deterministically.
### Inventory of serialized components
- Main CPU: `f3_cpu` registers (D0-D7, A0-A7, PC, USP, SSP, MSP, VBR, SFC, DFC, CACR, CAAR, SR, stopped, halted), native lazy flags (`cc_src`, `cc_dst`, `cc_result`, `cc_op`, `cc_width`, `cc_mask`), and `dispatch_deadline`.
- Machine Clocks and Scheduler: `hardware_cycles`, `next_vblank`, `irq3_at`, `watchdog_at`, `frame`, `pending_irqs`, `timer_control`.
- Inputs and Coins: input ports 0-5, `system_inputs`, `coin_count` [0..3], `coin_locked` [0..3], `coin_word` [0..1].
- Memory Regions: Main RAM (128 KiB), Palette RAM (32 KiB), Graphics RAM (256 KiB), Control RAM (32 B), Shared DPRAM (2 KiB), Display Framebuffer (320x232 ARGB8888, 296,960 B).
- EEPROM: 93C46 words (64x16), mode, pin latches, shift register, bit counter, address, ready deadline.
- Audio Subsystem:
  - Work RAM (64 KiB), bank mask, bank table (32 voices), reset/halt lines, gain model, volume/output gains.
  - Mixer accumulators: CPU accumulator, DUART accumulator, sample accumulator, clock ticks, generated frames.
  - Queued PCM: ring buffer active frames in playback sequence, remainder zero-filled to fixed 32,768-frame capacity.
  - ES5505 OTIS: master clock, sample rate, active voices, page, IRQ vector, mode, voice index, bank table, and 32 full voice channels (control, pitch, addresses, volumes, envelopes, BQ filters).
  - ES5510 DSP: halt line, PC, state, 192 GPRs (24-bit), 160 microcode instructions (48-bit), 1M-word DRAM (2 MiB delay RAM), serial sample registers, ALU/MAC/RAM pipeline stages, and host latches.
  - MC68681 DUART: control/status registers, baud/timer presets and remaining clocks, half-period latch, channels A/B mode and command registers, and serial transmitters.
  - MB87078 Electronic Volume: channel latches, gain indexes, control, and data registers.
- Sound CPU:
  - Native mode (`SoundNative`): `f3_cpu` registers, lazy flags, dispatch deadline, reset state, and reset debt cycles.
  - Oracle mode (`Interpreter`): Musashi 68000 architectural registers, status register, interrupt masks, internal flags, prefetch registers, virtual IRQ lines, and reset state. Pointer tables and host callbacks are restored in-process.
- Retained Video State:
  - FDP Renderer (`Video`): buffered spriteram (64 KiB), sprite framebuffer (432x256 indexed, 221,184 B), sprite priority row usage, 1024-entry tempsprite list, control registers, and row usage maps. Derived line caches (`last_y`) are invalidated upon load to ensure deterministic regeneration.
  - Enhanced Renderer (`GameVideo`, when configured): GameTiles tile maps (4 layers x 2048 cells) and layer validity, GameText cells (4096) and glyph RAM, GameSprites staging, submitted, and current sprite buffers (1024 each) with scroll registers, GameLines profile parameters and scene row calibration. Native pixels, sprite plane, and both expanded presentation buffers are serialized; restoring never substitutes nearest-neighbor pixels for rerasterized output.

## GPU presentation snapshot

The C CPU ABI remains version 2; generated hooks, lazy flags, bus callbacks
and scheduler are unchanged. `GameVideo::enable_gpu_presentation` enables a
host-only scanout snapshot exported by `gpu_scene()`. It captures semantic
cells, normalized rows and the **rendered** sprite list before the next latch.
GPU resources, packed shader data and CPU diagnostic caches are not serialized.
`render_reference` rerenders that snapshot through the retained CPU compositor,
with optional isolated layer masks and serial dispatch for parity/measurement.

Native `Machine::pixels` is still CPU-produced every frame. Expanded GPU
presentation avoids the per-frame expanded CPU raster; `presentation()` and
`save_state()` materialize the exact CPU presentation and next sprite plane
lazily when requested, preserving the existing canonical snapshot byte layout.
Load invalidates GPU host caches and initially presents the restored native
frame; the next scanout rebuilds GPU scene data. Netplay retains scale 1/border 0.

Opt-in GPU interpolation adds only host-side `VideoInterpolation`/
`InterpolationFields` analysis and appended per-playfield metadata in the
**upload copy**. Each row/playfield has flags and four triples of anchored
local polynomial increments (source X, X zoom, vertical phase, palette add).
No `SceneRow`, `GameLines`, machine-state, snapshot, CPU ABI or netplay schema
changes. Original `GpuScene` and lazily materialized CPU images remain
non-interpolated. Both interpolation modes preserve native subrow-zero samples;
alpha and discrete clip/mosaic/priority/column boundaries remain native.

## Runtime GPU scale

`GameVideo::set_gpu_scale` and `GpuVideo::set_scale` change host geometry only.
The `GameVideo` constructor fixes canonical presentation-buffer sizes for the
lifetime of the machine; auto starts this configuration at scale 1. Canonical
save/load bytes, CRCs, native sprite lag and expanded trail history are retained
while the selected GPU scale changes. Fixed and selected CPU-reference planes
share decoded scene sources but not scale-dependent raster storage.

GPU device/assets/pipelines and immutable tile pen masks survive scale changes.
Only render targets and already-used diagnostic readback buffers are replaced;
SDL defers releasing queued GPU resources. Scale/interpolation/blit policy is not
serialized. Netplay still requires fixed scale 1/border 0; no CPU ABI, machine
state or snapshot schema cutover.

### Sprite sampling verification

Sprite scale and flip already feed the internal-resolution raster directly.
Phase 8 retains the existing `SceneSprite`, GPU word layout, fixed-point raster
phases, producer hooks and canonical state. Diagnostic captures now include
all nine isolated layers; native-producer boundary branches cover crushed
opaque overlap, mirrored zoom and nominal top-edge culling. No new sprite
mode, transform field, CPU ABI, snapshot or netplay schema is introduced.
