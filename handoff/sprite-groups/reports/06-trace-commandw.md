## Result: origin X/Y writes in commandw's object draw all resolve to the record (a6)

Within `sub_009b32` (0x9b32..0xa192), three store instructions write X/Y words. Each one is a `move.l dN,(a4)+` whose high word is X and low word is Y. All six X/Y words trace to a direct `d(a6)` leaf, with ROM tables supplying only offsets. Piece entries get no X/Y stores. Identity is mechanically derivable inside this routine, but the record pointer a6 itself comes from a RAM list that I did not trace.

No repo files were touched. I only used the eval kernel (`f3a`, `dis`, `writes`) and read the caller's disassembly.

### Scan results
- **Store scan:** I checked every memory-destination instruction in the range. The only stores are the 21 sprite writes (through a4) plus the counter `-$7cb0(a5)` (0x408350), which counts entries.
- **a6 and a4:** a6 is never written inside `sub_009b32`. a4 is only advanced (`addq #8`, `(a4)+`).
- **Callees:** the six jump-table targets (0xa194, 0xa198, 0xa1a0, 0xa1aa, 0xa1b4, 0xa1b6, 0xa1bc, 0xa1bc) only move a0/a2 and load d0 from the stream. None store to sprite RAM, and none touch d3/d4.

### Table: origin X/Y writes

| Path (gate) | Write PC | Words | Leaf chain | Verdict |
|---|---|---|---|---|
| Shadow ($1(a6) b7 and global 0x408329 b0) | 0x009c90 `move.l d4,(a4)+` | +4 X_s, +6 Y_s | X_s = (`$44(a6)` − ROM word `T[B][2·$43(a6)]`) ±ROM term (index `$40`, `$43`) & fff. Y_s = (`$46(a6)` − ROM word `T[A][2·$42(a6)]`) ±ROM term (index `$42`). T = 0x6a000 pointer table; B and A are the second and first words of the 0x5a02c stream. | RESOLVED to a6+$44 (X) and a6+$46 (Y) |
| Body, single part (stream count byte 0 or 0xff) | 0x009e9a `move.l d4,(a4)+` | +4 X, +6 Y | X = `$6(a6)` − `T[B][2·$5(a6)]` ±ROM term (index `$2`). Y = `$8(a6)` − `T[A][2·$4(a6)]`. | RESOLVED to a6+$6 (X) and a6+$8 (Y) |
| Body, multi-part (per sub-part; count = stream byte) | 0x00a0be `move.l d4,(a4)+` | +4 X, +6 Y | As single-part, with ±sign selected by `$3(a3)` bit 0 (X) and bit 1 (Y), and `$0(a6)` bit 8. Sub-part index is `(a3)` through 0x5a02c. | RESOLVED to a6+$6 and a6+$8 |

Non-X/Y origin words (tile, zoom, attr) are listed for completeness:
- 0x009c8a: tile from the ROM stream (indexed by `$40`), zoom from `$42(a6)`.
- 0x009e94: tile from the ROM stream (indexed by `$2`), zoom from `$4(a6)`.
- 0x00a0b8: same as 0x009e94, for the multi-part header.
- 0x009ca8, 0x009eaa, 0x00a0dc: attr at +8 from `$0(a6)&0x300`, `$a(a6)`, and constants.

**Coverage:** 3 of 3 X/Y store instructions, and 6 of 6 X/Y words, resolve to a direct `d(a6)` leaf. The ROM tables are indexed by record bytes or stream words, which is the rigid-offset case from the trace definition.

**Block members:** piece entries (0x009cd6, 0x009cda, 0x009ed8, 0x009edc, 0x00a10e, 0x00a11a) store only tile at +0 and attr at +8. Words +2, +4, +6 are never stored by this routine, so members carry no X/Y from this code. The caller's end-of-frame fill (0x009a7a..0x009ac2) zeroes only slots beyond the current entry count, up to the previous count. Member X/Y words inside the current count therefore keep stale values from earlier frames. [INFERENCE: that is how the hardware uses them; not verified.]

### Discrepancies with the assignment's known facts
- **X index:** X uses `T[B]`, the second stream word (`move.w d6,d5` at 0x9b6c, 0x9d6a, 0x9f56). The earlier claim of `T[d7]` for X is wrong. Y uses `T[A]` (d7).
- **Shadow anchor:** there is no single "$44 long" anchor. Word `$44` is the X base and word `$46` is the Y base. `$43` is the X table index, not zoom. The shadow zoom is `$42`, which is also the Y table index.

### Open dependency: where a6 comes from
- The caller (`sub_00994a`, 0x9a18..0x9a34) loads a6 from a RAM list at 0x40885a (up to 0x79 entries, read downward, −1 entries skipped).
- When the list runs out, it falls back to 0x413c00 with stride 0x80 for eight records.
- So a6 is a RAM-loaded record pointer in the list case, not a static `0x410000 + k·0x80`. Its identity is the value of a6. I did not trace who writes the list at 0x40885a.

### New tracer rules needed
1. **Two-level ROM pointer tables:** `lea ROM,aN; adda.w idx,aN; movea.l (aN),aN; adda.w 2·rec_byte,aN; move.w (aN),dX`. The index can be a stream word.
2. **Sign-extended indexes:** `adda.w` sign-extends the stream word used as index.
3. **Jump-table dispatch:** `jsr (a1)` with a1 from ROM tables 0xa30e (`(a6)&3`) and 0xa31e. The targets only move a0/a2 and clobber d0. Tracer must treat them as a0-advance with no stores.
4. **Reverse stream reads:** `move.w -(a0),…` is selected by `$0(a6)` bit 9.
5. **Sign by branch:** `add` vs `sub` chosen by `btst` on `$3(a3)` bits, `$0` bit 8, and `$0` bit 11. Leaves from both branches are unioned.
6. **Long-lane split:** `swap` then `move.w` builds `[X:Y]` or `[tile:zoom]`.
7. **Loop-carried cursors:** a3 steps 4 bytes per sub-part, and the sub-part loop counter comes from the stream byte at table[`$2`]. a4 is never reset per unit.
8. **Nested piece loop (shadow):** the outer `dbra` counts `d6.w` (= B). The inner counter is reloaded from d7, not d7−1, at 0x9d16. This gives A pieces in the first row and possibly A+1 in later rows. [INFERENCE: from the listing, not dynamically checked.]
9. **Bank select:** `0x408013` bit 0 picks a4 = 0x600340 or 0x608340 (0x9968..0x9978).

### Checks for Main to run
- Dynamic check of the piece count per row (rule 8).
- Trace the writer of the 0x40885a list to close the a6 provenance question.