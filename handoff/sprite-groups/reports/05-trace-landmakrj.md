## Result: static backward trace of landmakrj sprite X/Y writes

**Verdict:** Automatic record-field resolution works for the grid, 3-tile and 4-tile handlers (18 of 22 X/Y write instructions, 18 of 26 word lanes). It does not work for the scaled handler (0xa913c), which resolves its base to the record but whose increments are data-dependent. Every X/Y write in the four handlers resolves its base to `a3+$c` / `a3+$e`.

### Per-write table (normal path, sprite address from f3a where linear)

| handler | write PC | X/Y | sprite word | leaf chain | verdict |
|---|---|---|---|---|---|
| grid 0xa8f38 (normal, d3 bit8=0) | 0x8f5e | X | entry+4 | d1 ← `move.w $c(a3),d1` (0x8e28) & fff; +16 per row (0x8f70 addi #$10) | RIGID: a3+$c, const pitch |
| grid | 0x8f62 | Y | entry+6 | d4 ← d2 (`$e(a3)` 0x8e2c, andi fff 0x8f3e) + 16 per col (0x8f68) | RIGID: a3+$e, const pitch |
| grid flip 0x9036 (entered 0x8f48) | 0x9052 | X | entry+4 | d1 ← $c(a3) + 16·d7 − 16·row; d7 = word at `(a0)+2`, a0 = [a3] (two-level) | RIGID-per-object: `[[a3]+2]` term, const pitch |
| grid flip | 0x9056 | Y | entry+6 | d4 ← $e(a3)&fff + 16/col | RIGID |
| 3-tile 0xa8f84 (gate `$6(a0)≠0`, a0=[a3]) | 0x8f98 | X0 | +4 | ($c&fff)+5 (addq 0x8f96) | RIGID |
| 3-tile | 0x8f9e | Y0 | +6 | $e&fff | RIGID |
| 3-tile | 0x8fb2 | X1 | +4 | $c&fff | RIGID |
| 3-tile | 0x8fb4 | Y1 | +6 | ($e&fff)+16 (0x8fae) | RIGID |
| 3-tile | 0x8fc4 | X2 | +4 | ($c&fff)+16 (0x8fbe) | RIGID |
| 3-tile | 0x8fc6 | Y2 | +6 | ($e&fff)+16 | RIGID |
| 4-tile 0xa90f4 (gate `$6(a0)≠0`) | 0x9100 / 0x9102 | X0 / Y0 | +4 / +6 | `$c(a3)` / `$e(a3)`, unmasked | RIGID |
| 4-tile | 0x9112 / 0x9114 | X1 / Y1 | | `$c` / `$e+16` | RIGID |
| 4-tile | 0x9124 / 0x9126 | X2 / Y2 | | `$c+16` / `$e` | RIGID |
| 4-tile | 0x9132 / 0x9134 | X3 / Y3 | | `$c+16` / `$e+16` | RIGID |
| scaled 0xa913c, V00 (bits 0/1 of `$13` = 0,0) | 0x91ea (long X:Y) | X,Y | +4,+6 | seed d1 = (a5.w<<16)\|$c(a3); pre-loop += ((`$10`byte·(H_lo+1))+8)>>5 with H = [[a3]] header long; per-row mulu/ror/swap accumulator | BASE RESOLVED, increments NOT rigid |
| scaled V01 | 0x92e4 | X,Y | | same family, sub-based steps | BASE RESOLVED, NOT rigid |
| scaled V10 | 0x926a | X,Y | | same family | BASE RESOLVED, NOT rigid |
| scaled V11 | 0x9368 | X,Y | | same family | BASE RESOLVED, NOT rigid |

Scaled-handler detail: d1.hi is seeded from a5.w (0x9168–0x9172), and the fractional carry uses that high word. The `$12` hi byte drives the Y pre-loop term. The `$13` bits 0/1 select the four variants. The carry semantics are [INFERENCE].

### Coverage

- Grid: 4/4 X/Y instructions resolve to `a3+$c/$e`. Normal path is constant; flip path has a per-object term.
- 3-tile: 6/6 resolve, constant offsets.
- 4-tile: 8/8 resolve, constant offsets, no masks.
- Scaled: 4 long writes (8 word lanes) resolve to base, 0 rigid.
- Overall: 22/22 instructions (26/26 lanes) reach the record base; 18/22 instructions (18/26 lanes) are rigid.
- Bonus, queue single handler 0x4688: X 0x469a and Y 0x46a2 come from the caller's stream `(a4)` (+4, +6 relative to entry a4), not from a3. 0/2 record-resolved. Word0 comes from `(a0)` (+2), attr ^= `(a0)` (+0) `[INFERENCE: a4/a0 caller base mapping to +8/+10 not verified]`.
- Bonus, grid queue 0x46c0: 24 sprite write sites across 4 shared tails. Not traced for X/Y chains (out of budget). Observed: shared tails 0x487c/0x48fe/0x4976/0x49f8 write `move.w a3,(a6)+`, i.e. the low word of the record pointer itself goes into sprite RAM (f3a labels 0x600338/0x60033c/0x600340/0x600348). This is a direct-key case `[INFERENCE: label correctness on shared tails]`.
- Bonus, commandw block-origin 0x9b32..0xa192: not traced. The dis call returned only 42 lines with no sprite writes, so no result.

### Handler selection (slot +8 code pointer)

- Stores of code pointers into `$8(reg)` (xref): #$a8f84 at 0x955c6; #$a8f38 at 0x95734 and 0x96162; #$a90f4 at 0x95d5a and 0x96922; plus #$a8f38 at 0xa8efc (base 0x8 rom data, anomalous).
- In most of these, the base register is a long loaded from `a3+$14`, so the write target is `[[a3+$14]+8]`, not `a3+8`. Whether those coincide is open `[INFERENCE]`.
- No static store of #$a913c was found in the printed xref; scaled is reachable only through slot +8 (caller_sites=0), consistent with never seen at runtime.

### Special rules a generic tracer needs

1. Indirect dispatch: `jmp ([$8,a3])`. Slot +8 is written at runtime; xref sees only constant stores; the base is itself loaded from `a3+$14`.
2. Return via a4 (`jmp (a4)`), set by the dispatcher to 0xa8e40. No call/ret; the tracer must treat a4 as a continuation.
3. Two-level pointers: `a0 = [a3]` (long at +0) is a stream/header pointer. Tile/attr/flip terms come from `[[a3]+k]`; X/Y base comes from a3 directly, except in scaled, where the header also seeds steps.
4. Sprite cursor a1 is global: loaded from `-$5de(a5)` (0x407a22) on dispatch entry, stored back at loop end. Entry address is not a function of the record; key must come from the value's a3 provenance, not the address.
5. Queue: a3 = `(a5 & 0xffff0000) | word` from list at a5+$29b0 (0x40a9b0), clear-on-use (`clr.w -2(a2)`). a5 = 0x408000 (inferred from the list address), so a3 = 0x40xxxx.
6. Sparse writes: `beq` on stream long = 0 skips writes and the cursor advance (grid 0x8f56; scaled 0x91da). Write count and slot compaction are data-dependent.
7. Early-out gates: 3-tile and 4-tile skip all writes when `$6(a0)=0`, returning without advancing the cursor.
8. Flip selection: grid tests d3 bit8 at 0x8f48. Dispatcher sets bit8 from `tst.b $13(a3)` via `bset.b #8,d3`. Scaled discards that bit (`andi #$ff` plus `move.w d0,d3` composition) and uses `$13` bits 0/1 instead.
9. Long-size writes composed of two word lanes (scaled: `move.w d1,d0; swap; move.w d2,d0; move.l d0,(a1)+`). The tracer must split longs into lanes.
10. Masking inconsistency: grid and 3-tile use `andi.w #$fff`; 4-tile does not; scaled masks at 0x91b8/0x91bc.
11. Accumulator classes: grid/3/4 use `addi #16` (constant, rigid). Scaled uses mulu and carry-based fractions (not rigid).
12. Record pointer as data: queue handlers can write `a3.w` into sprite RAM (0x46c0 shared tails).
13. f3a tool issue: `stepped +N` labels assume linear order. On the grid flip path (entered via `bne.w` at 0x8f48, a1 unmodified) f3a labels 0x600320.. are off by +16; true base is 0x600310 `[INFERENCE from control flow]`. Shared tails are also mislabeled.

### Repo state
`git status` fails in the worktree (submodule path error), so repo state is not git-verified. I wrote no files and made no edits. The three pre-existing user changes (docs/notes/raw/docs/taito-f3, runtime/frontend/state.hpp, runtime/rom.cpp) were not touched. Scratch used only the eval kernel; no /tmp files.

### Checks for Main
None run (read-only assignment). Verification suggestion: confirm #$a913c has no store anywhere (full xref, not truncated) and compare scaled's word lanes against one runtime capture if scaled is ever observed.