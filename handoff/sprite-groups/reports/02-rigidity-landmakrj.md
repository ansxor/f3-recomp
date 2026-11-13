## Result: landmakrj display-object handlers are RIGID within a group

Group-only interpolation is valid for landmakrj: every sampled handler computes member positions from the object's base (+$c, +$e) plus constant or template-derived offsets, with no per-part RAM state. Verdict based on static analysis plus runtime evidence for 3 of 4 handlers.

**Handler list (distinct `$8(a3)` targets).** Static immediate stores with displacement 8, found by scanning ROM for `move.l #imm,8(An)`, and confirmed by a runtime scan of the object pool:

| Handler | Emitter | Static writers | Runtime (1P gameplay, frames 1880–3000) |
|---|---|---|---|
| `0x0a8f38` grid | `sub_0a8f38`, flip branch `0xa9036` | `0x0956d6`, `0x095734`, `0x095858`, `0x096162`, `0x0a8efc` | seen, 32–44 slots/frame |
| `0x0a8f84` 3-tile | `sub_0a8f84` | `0x0955c6` | seen, 27–54 slots/frame |
| `0x0a90f4` 4-tile | `sub_0a90f4` | `0x095d5a`, `0x096120`, `0x0965d0`, `0x096632`, `0x096922`, `0x0978ba`, `0x0978c2`, `0x0aa10a` | seen, 8–34 slots/frame |
| `0x0a913c` scaled | `sub_0a913c` | `0x0fba98` (`move.l #$a913c,8(a0)`) | not observed in either run |

Static analysis found only these four. The scan also turned up `move.l d0,$8(a3)` and `sub.l d0,$8(a3)` at `0x0967b4`/`0x096800`/`0x09680a` in `sub_095e7c`. Those values are fixed-point positions, not code pointers, so they belong to a different object family. A display object that held one would crash at `jmp ([$8,a3])`, and no crash occurred in either run, so I treat them as outside the display list `[INFERENCE]`.

**Per-handler evidence**

- **Grid `0xa8f38`: RIGID.** Origin is X=$c, Y=$e, attr is d3. Tile pitch is the constant `addi.w #$10` at `0xa8f68`/`0xa8f70`. Dimensions and tile codes come from the template at +0. The flip branch (`btst #8,d3` at `0xa8f44` → `0xa9036`) mirrors x using the template's column count. Runtime sample: slot `0x4091f0`, template `0x56d70`, base (80,47), constant over frames sampled. A zero tile skips the emit but still advances y.
- **3-tile `0xa8f84`: RIGID.** Template words $4 (eor attr), $6, $a, $e. Parts are A=(X+5,Y), B=(X,Y+0x10), C=(X+0x10,Y+0x10), all immediate offsets. Skip-all if $6==0. Runtime: 57 samples per slot, template `0x56878` fixed, attr 0xa4 fixed, flag13 0. Base moves between frames (e.g. slot `0x408cf0`: x 26–27, y 47–63), but descriptor, attr and flag do not change, so the offsets stay fixed.
- **4-tile `0xa90f4`: RIGID.** Parts A=(X,Y), B=(X,Y+0x10), C=(X+0x10,Y), D=(X+0x10,Y+0x10), all immediates. Skip-all if $6==0. Runtime sample `0x4090b0` has $6==0, so this sample emits nothing `[INFERENCE]` that it is a placeholder or empty slot.
- **Scaled `0xa913c`: RIGID under object zoom, not runtime-observed.** The only RAM reads are the object's own fields (`$10`, `$12`, `$13`). Per-part offsets are accumulated from the descriptor and scaled by that zoom. No per-part state is read, so members stay rigid relative to the object's zoom. Remaining code (`0xa9200..0xa93a4`) has only register-relative jumps through a4, no further RAM reads. Runtime never produced this handler, so it is `[INFERENCE]` from static reading.

**Member count and order variation.** Member set changes with the descriptor, not with motion:
- Zero tiles in the descriptor skip emission (grid, 3-tile, 4-tile).
- The 3-tile handler applies `eor.w d4,d3` to attr persistently, so later members get different attr values.
- The flip bit (`bset #8,d3` when `$13` is nonzero) reverses grid emit order and x stepping.

These change which sprites are emitted and their attributes, not their offsets from base.

**Runs.** Two f3a runs wrote dumps under `build/f3a/landmakrj-disp-handlers` (attract, 1500 frames) and `build/f3a/landmakrj-disp-1p` (1P gameplay, 3000 frames, `coin-start-mash.txt`). Neither modifies the repo. The display-object list at `0x40a9b0` is cleared by the loop each frame, so I scanned the object pool instead.

**Verdict for group-only interpolation:** pairing groups only is sound for landmakrj's display objects. Caveats: member count, zero-tile skips, flip and per-part attr changes can alter the emitted sprite set, so interpolation must tolerate a member set that differs between frames.

Checks for main to run: none. I ran no builds or tests.