## Result: commandw `objects` body is RIGID per object; the shadow is a separate positional group

**Verdict:** Body parts are rigid relative to one object anchor for a given zoom. Each part's offset is a ROM-table function of the object's zoom bytes and the descriptor's dimensions, with no per-part RAM state. The shadow is not a fixed child of the body. It is a second projection of the same object, with its own anchor and zoom recomputed every frame, and it exists only on alternate frames. Pair the shadow as its own group, not as a rigid member of the body group.

Verified with f3a `dis` over 0x9a3c..0xa2f8 and generated C in `build-commandw/generated/commandw/blocks_0004.c`. Anything not directly observed is marked `[INFERENCE]`.

### Per-object pipeline (0x9a3c span)
- 0x9a3c: `btst.b #7,(a6)`; skip the object if clear. This tests byte **+0** bit 7, but `behaviours.hpp` documents byte **+1** bit 7 as the shadow flag (tested at 0x9b38). Both are real; they are different bits.
- 0x9a4c/0x9a5a: projection `0xa1c8` (or `0xa3ee`).
- 0x9a64: `0xa334` LoD zoom rescale (per `behaviours.hpp`, `$4 = ($4 − T[k]) << k`).
- 0x9a68: `0x9b32` shadow + body emit. Record stride 0x80 (0x9a6c).
- Generated C: `L_009a3c` at `blocks_0004.c:26976`; `L_009b32` at `:28133`.

### Step 1 — Body tile positions (0x9d32 / 0x9d5c / 0x9f38)
- Projection `sub_00a1c8` (0xa1c8..0xa2f0) writes the per-frame object anchor and zoom:
  - `$4` and `$5` are both the zoom `d6 = min(0x40000 / depth, clamp 0xe0)` (0xa246/0xa24a, 0xa2d4/0xa2d8 for the shadow).
  - `$6` long is the anchor (X = `$6` word, Y = `$8` word) (0xa25a). Inputs: `$20` long, `$24` word, camera words 0x409036..0x409042.
- Body part position, single-grid path 0x9d5c, with `d7 = desc.word0`:
  - `X = ($6 − T[d7][$5]) & 0xfff`
  - `Y = ($8 − T[d7][$4]) & 0xfff`
  - `T[d7]` = `*(0x6a000 + 4*d7)` (a 256-entry word table per dimension). Sign flip from object word0 bit 8 (0x18). The bit-11 (0x1b) branch adds a second term from 0xc9000 and 0x6b870, keyed on `$2` and `$4`/`$5`. What that term means is `[INFERENCE]`.
- Multi-part path 0x9f38: each sub-part uses its own dimensions `d7 = sub.word0` plus flip bits `$3(a3)` bit 0 (X) and bit 1 (Y). Same formula. No per-part RAM input.
- Per-part header is written once: word1 = zoom (`$42`/`$43` for the shadow, `$4` for the body), word2 = X, word3 = Y, word4 = attributes (0x9c8a/0x9c90, 0x9e94/0x9e9a, 0xa0b8/0xa0be). Only the first entry carries a position. Rest of the grid is tiles plus attribute words (0x9cd6, 0x9d04). That matches the F3 block model in `docs/notes/hardware/sprites.md` (only the first sprite holds a position; block controls place the rest).
- Not verified: the exact placement of 0xc9000/0x6b870 terms and the meaning of `$48` (see Step 2).

### Step 2 — Shadow offsets $42–$44 (per-frame, not constant)
- Writers, from `xref`:
  - `$42`, `$43`: `sub_00a1c8` at 0xa2d8 / 0xa2d4 (`move.b d6,$42(a6)` / `$43`), zoom values.
  - `$44`: `sub_00a1c8` at 0xa2e8 (`move.l d3,$44(a6)`), anchor long.
  - `$40`: 0x2fee6 and 0x2fef4 (`sub_02fed6`, LoD descriptor select, see `behaviours.hpp`).
- Shadow anchor (0xa268..0xa2e8): the same world inputs as the body (`$20`, `$24`, camera), but the low coordinate is replaced by `$48`. Depth divide is repeated, so the shadow point is a separate perspective projection.
- Shadow position therefore varies per frame and is not a fixed offset from the body anchor. It cannot be expressed as `body + constant`.
- Shadow gating: 0x9b38 (`btst #7,$1(a6)`) then 0x9b42 (`btst #0,-$7cd7(a5)`, 0x408329 counter parity). Shadow exists only on odd counter parity, so the shadow group appears and disappears on alternate frames. Interpolation must treat it as present or absent per frame.

### Step 3 — Articulation search (negative)
- In 0x9b32..0xa192 the only per-object reads are the object record (`$0`–`$a`, `$20`–`$48`) plus global camera/flags (0x408956, 0x408255, 0x408328, 0x408350 counter). No parent pointer, no per-part angle, and no per-part RAM variable.
- The multi-part path reads only descriptor constants (`$2(a3)`, `$3(a3)`) and the same object fields.
- Subroutines 0xa30e and 0xa31e (indexed by `byte0 & 3`) only adjust the descriptor pointer `a0` or `a2` (`addq #6,a0`, `a0 += word`, `a0 += $2(a0)`, `a0 += $4(a0)`, or `a2 ± d2`). They write no sprite RAM.
- Conclusion: no articulation in this draw path. Parts differ only by constant descriptor data and flip bits.

### Step 4 — Tile emission order
- Fixed row-major loops: `dbra` counts come from descriptor words. Per row, `d7` tiles are read via `(a0)+` or `-(a0)` (direction from object word0 bit 9, 0x19), then the row pointer is adjusted by `0xa31e` (`byte0 & 3`: 0 and 3 no-op, 1 back one row, 2 forward one row; row stride `d2 = (cols+1)*2`).
- No per-tile value-based skip. Only branches: `tst.w d7 / beq` (zero columns skips the row, 0x9cc8/0x9eca), and `(d6|d7)==0` (single tile, 0x9cb4/0x9eb4, which patches the previous entry).
- Tile codes are emitted unconditionally.
- Off-by-one on column count `[INFERENCE]`: the inner loop runs `word0` times as written; row count is `word1 + 1`.

### Notes for the landmakrj comparison
- Sprite entry layout is 16 bytes: word0 tile, word1 zoom, word2 X, word3 Y, word4 attribute (per `docs/notes/hardware/sprites.md`).
- Open items: meaning of the 0xc9000/0x6b870 adjust term; meaning of `$48`, `$20`, `$24`, and camera 0x409036..0x409042. Marked `[INFERENCE]`.
- Generated-C line refs: `L_009a3c` `blocks_0004.c:26976`; `L_009b32` `:28133`; shadow-zoom PC 0x9b4c `:28320`; body PC 0x9d32 `:30675`; `L_009d5c` `:30903`; `L_009f38` `:33228`; projection `L_00a1c8` dispatch `:36452`. The 0xa2d4/0xa2e8 writes are cited by f3a only, not by line.

No repo files were edited. No builds or tests were run.