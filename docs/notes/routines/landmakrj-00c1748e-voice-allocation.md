---
title: Land Maker sound driver voice allocation and lifecycle (c1748e)
created: 2026-10-12
updated: 2026-10-12
type: routine
tags: [audio, sound-driver]
sources:
  - raw/docs/runtime-audio-sequences.md
  - raw/disasm/landmakrj-sound-driver-excerpts.md
  - raw/tests/landmakrj-seed5-sound-trace.md
games: [landmakrj]
addresses: [0x00C1748E, 0x00C1753A, 0x00C1762C, 0x00C17632, 0x00C17664, 0x00C17670, 0x00C17678, 0x00C176F8, 0x00C17B94, 0x00C17E62, 0x00C177F6, 0x00C17814, 0x00C1781C, 0x00C1789A, 0x00C17A88]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/runtime-audio-sequences.md, note: "runtime documentation of voice allocation algorithm, priority classes and list flow"}
  - {kind: doc, ref: raw/disasm/landmakrj-sound-driver-excerpts.md, note: "ROM disassembly of voice allocation search ($c1748e), legato reuse ($c1753a..$c17664), priority exhaustion ($c17670/$c17678), carry rejection ($c176f8), voice init ($c17b94), OTIS setup ($c17e62), pending trigger loop ($c177f6/$c17814), list splice ($c1781c), and voice freeing ($c1789a/$c17a88)"}
  - {kind: trace, ref: raw/tests/landmakrj-seed5-sound-trace.md, note: "seed-5 trace: 1,639 note allocations and 2,202 non-startup voice starts; legato ownership changes observed without CR transitions"}
contradictions: []
supersedes: []
---

# Sound driver voice allocation and lifecycle

How the Land Maker sound driver allocates, reuses, programs and silences physical ES5505 voices.
Hardware playback is on [[hardware/es5505-otis]]; sequencing and table formats are on
[[routines/landmakrj-00c14730-sequence-event-decode]]; command dispatch is on
[[routines/landmakrj-00c12ecc-sound-command-dispatch]]; game context is on [[games/landmakrj]].

## Voice search and legato reuse

- Allocation is not a simple oldest-voice replacement rule: routine `$c1748e` first searches matching effective-key, channel and layer voices in the released list at `$d850` and the free list at `$d844`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Retrigger and legato paths `$c1753a..$c17664` may reuse an existing physical voice and replace its note-node owner without issuing a fresh hardware key-on or stopped-to-running CR edge. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- The owner change at `$c1762c` (`move.w $d81c.w, $e(a4)`) is visible at the immediately following `$c17632` write (`move.w $d82c.w, $a(a4)`), updating voice ownership in work RAM without touching OTIS control registers. ^[raw/disasm/landmakrj-sound-driver-excerpts.md] ^[raw/tests/landmakrj-seed5-sound-trace.md]

## Exhaustion scan and priority classes

- When matching search and free list `$d844` are exhausted, execution branches to `$c17670/$c17678` to scan the released list at `$d850` and the active list at `$d848`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Priority class comes from patch descriptor `+$60` bits 6–7 (`and.w #$c0, d1` at `$c17674`) and is saved in voice record `+$97(a4)`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- The scan prefers priority class zero, then class `$40` when permitted, then other eligible list heads. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Flag checks at `$c176f8` can reject an allocation request with carry set, producing no allocated voice. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]

## Voice initialization and OTIS programming

- Routine `$c17b94` initializes a selected physical voice record at `$d94a` (stride `$aa`): it clears state, copies the note node at `$d81c` to voice `+$0e` (at `$c17bc6`), copies channel pointer `a2` to voice `+$10` (at `$c17be0`), and loads scaling factor table `$c08bec` into voice `+$8e`. ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Routine `$c17e62` programs the hardware: it selects the voice's OTIS page, zeroes volumes (`move.l d0, $10(a0)`), stops the voice by setting CR stop bits 0–1 (`ori.w #$3, (a0)`), initializes filters and accumulator (`move.l d0, $c(a0)`), and programs sample start/end/control parameters. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]

## Pending trigger list and active splice

- Pending voice starts are linked into the pending list under `$d84c`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Routine `$c177f6` walks the pending list at `$d84c`: for each pending voice, `$c17814` clears CR stop bits 0–1 (`andi.w #$fffc, (a0)`) to trigger hardware playback, unless voice-state high-byte bit 4 suppresses the trigger (`btst.b #4, $c(a4)` at `$c1780c`). ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Routine `$c1781c` updates rotating update links at `+$24/+$26(a4)` and splices the pending list into the active list at `$d848`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- The `$d85a` ring schedules recurring per-voice pitch, filter and volume updates (`$c18360/$c18a2e`) following key-on. ^[raw/docs/runtime-audio-sequences.md]

## Voice release and hardware silencing

- Routine `$c1789a` unlinks a freed voice from active/released lists, returns it to the free list at `$d844`, and silences/reinitializes the hardware voice through `$c17a88`. ^[raw/docs/runtime-audio-sequences.md] ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Routine `$c17a88` stops playback on the physical OTIS voice and resets voice registers. ^[raw/disasm/landmakrj-sound-driver-excerpts.md]
- Mailbox `0x8f` releases matching notes through `$c141ce`, transitioning active voices to the released list at `$d850`. ^[raw/docs/runtime-audio-sequences.md]
- Seed-5 6000-frame trace observation: 1,639 note allocations; 2,202 non-startup voice starts (277 direct, 1,925 sequenced), plus two silent startup voice-0 probes. ^[raw/tests/landmakrj-seed5-sound-trace.md]
