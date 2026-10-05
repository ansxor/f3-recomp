# Flags and timing

Generated instructions share a condition-code contract and an absolute cycle clock. These rules permit long native blocks without delaying scheduled events.

Sources: [`cpu_ops.h`](https://github.com/ansxor/f3-recomp/blob/main/recomp/cpu_ops.h), [`timing.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/timing.py), and [`generate.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/generate.py).

## Lazy NZVC and eager X

The low SR bits are C=`0x01`, V=`0x02`, Z=`0x04`, N=`0x08`, and X=`0x10`.

`cc_op`, `cc_src`, `cc_dst`, `cc_result`, and `cc_width` describe pending flags. Width is one, two, or four bytes.

| Pending operation | Flush behavior |
| --- | --- |
| `F3_CC_OP_NONE` | SR is already canonical. |
| `F3_CC_OP_LOGIC` | Set N/Z from the masked result; clear V/C; preserve X. |
| `F3_CC_OP_ADD` | Compute carry, signed overflow, N/Z, and matching X/C. |
| `F3_CC_OP_SUB` | Compute borrow, signed overflow, N/Z, and matching X/C. |
| `F3_CC_OP_CMP` | Use subtraction NZVC, but preserve X. |

`f3_cc_flush` materializes those flags into SR and clears `cc_op`. It preserves the other SR bits.

ADD, SUB, and NEG update X immediately. This makes X valid even when their NZVC calculation remains pending.

A new complete NZVC producer can replace a pending producer. An instruction that needs old flags first flushes them.

ADDX, SUBX, NEGX, and decimal operations need old X and cumulative Z. They set flags eagerly. A zero result retains old Z; a nonzero result clears it.

Shifts, rotates, multiply, divide, bit tests, and bitfields use helpers with canonical flag handling. Operations that change only some flags preserve the others.

Only memory callbacks accept pending flags. Hooks, exceptions, fallback, SR changes, reset callbacks, and block returns require canonical SR.

## Conditions

`f3_eval_cond` flushes before reading SR. `COND_MAP` supplies these four-bit condition numbers:

| Number | Condition | Expression |
| --- | --- | --- |
| 0 / 1 | T / F | true / false |
| 2 / 3 | HI / LS | `!C && !Z` / `C || Z` |
| 4 / 5 | CC, HS / CS, LO | `!C` / `C` |
| 6 / 7 | NE / EQ | `!Z` / `Z` |
| 8 / 9 | VC / VS | `!V` / `V` |
| 10 / 11 | PL / MI | `!N` / `N` |
| 12 / 13 | GE / LT | `N == V` / `N != V` |
| 14 / 15 | GT / LE | `N == V && !Z` / `N != V || Z` |

Branches, DBcc, Scc, and conditional traps use the same map. DBRA maps to the false condition.

## Cycle metadata

[`68020_cycles.csv`](https://github.com/ansxor/f3-recomp/blob/main/recomp/68020_cycles.csv) contains `mask,match,cycles` descriptors. It contains opcode metadata, not ROM bytes.

The header names Musashi revision `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` and the MIT license attribution.

`_load_base_cycles` starts with 65,536 zero bytes. For each row, it enumerates every subset of the unmasked bits.

Each index `match | subset` receives that row's cost. Later rows overwrite earlier rows. The final immutable bytes object is `BASE_CYCLES`.

The [cycle exporter](https://github.com/ansxor/f3-recomp/blob/main/tools/differential/export_cycles.py) reproduces the metadata from the reference build.

[`68000_cycles.csv`](https://github.com/ansxor/f3-recomp/blob/main/recomp/68000_cycles.csv) uses the same descriptor shape for the sound CPU. Main-CPU timing does not load it.

## Additional costs

`instruction_cycles` returns a C expression, not a Python execution time.

- MOVEM adds three cycles per stored register, or four per loaded register.
- RESET adds 518 cycles to its base cost.
- Full indexed EAs add `FULL_INDEX_CYCLES[extension & 0x3f]` when bit 8 selects full format.
- MOVE checks both EA extensions. Only decoded memory operands enable EA extension charges.
- A short conditional branch costs two fewer cycles when it falls through.
- DBcc adds four cycles when decrement reaches `-1`.
- MOVES adds two cycles for memory-to-register transfers and long register-to-memory transfers.
- Successful CAS and CAS2 add three cycles.

Exception paths call the runtime exception helper. They do not also execute the normal trailing cycle statement after returning.

These are reference scheduling costs. They are not a bus-cycle-accurate model of physical hardware.

## MAME corrections

[Project notes](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/DECISIONS.md) record corrections found through observed native/MAME comparisons.

MOVEM store timing uses three cycles per register, not upstream Musashi's four. Load timing remains four.

EC020 rotate timing does not add a count-dependent charge. The shift/rotate helpers compute semantics, while opcode metadata supplies scheduling costs.

TRAP entry includes the four-cycle opcode charge. The runtime and patched reference core own this exception-cost correction.

Word DIV overflow clears C while retaining undefined N/Z. The helpers follow that reference behavior rather than replacing undefined flags arbitrarily.

## Deadline and resume

`cpu->cycles` accumulates instruction costs. `cpu->dispatch_deadline` is an absolute threshold published by the runtime.

After an instruction, the generated continuation guard tests the deadline, PC, stopped state, and halted state. On a failed guard, it flushes and returns.

An instruction can cross the threshold. The block yields at that instruction boundary, not during a memory access or arithmetic operation.

The updated PC identifies the next instruction. Every decoded instruction has a dispatch entry, so execution can resume inside the same native function.

Device-sensitive bus callbacks can synchronize devices before an access. This differs from boundary-only interrupt delivery. Correct opcode costs alone do not establish correct device access time.

See [Blocks and dispatch](/developer/recompiler/blocks-and-dispatch) and [CPU ABI](/developer/runtime/cpu-abi).
