# Emit-unit replay and sprite behaviours

Render-only machinery (nothing here is serialized or part of `Machine` state):

- `[[video.emit_units]]` / `[video.frame_writers]` in `games/<id>/config.toml`
  ([syntax](../site/reference/game-config.md#videoemit_units-and-videoframe_writers)).
- `runtime/sprite_units.{hpp,cpp}`: identity tracking, synchronous sandbox replay
  (copy-on-write RAM overlay, no emulated side effects), splices, check mode.
- `runtime/renderer/sprite_behaviour.hpp`, `games/<id>/sprites/behaviours.hpp`:
  per-game patches applied to the replay.

Units and behaviours are always on: the frontend creates `SpriteUnits` whenever the
game declares units and runs natively (`--translated`; the interpreter has no unit
hooks), and enables every registered behaviour. There are no flags. They never change
cycles, native block counts or `Machine::state_crc()` / `sync_state_crc()`. The
checker tools keep their own on/off switches for parity proofs.

## Workflow: add or change a unit

1. Find the loop that draws one object/record and note the span PCs and the register
   holding the unit address (see Command War and Land Maker configs for annotated
   examples).
2. Configure (`cmake -S . -B <dir>`; regenerates the hooks) and run the checker:

   ```sh
   cmake --build build -j10 --target f3rt-sprite-check
   ./build/f3rt-sprite-check --rom-dir ../roms/landmakr --frames 6000
   ```

   `--set` defaults to the compiled game; `--frames N` (default 6000);
   `--compare-every N` (default 60) is the state-CRC comparison interval;
   `--seed N` / `--no-inputs` control the coin/start/button-mashing schedule shared
   with `f3rt-gameplay-regression`; `--behaviour NAME` is repeatable.
3. Read the report. It must end in `PASS` (exit code 0):
   - every invocation `matched` (unpatched replay equals the real writes bit for
     bit), no aborts;
   - `sprite writes outside units ... (unaccounted 0)`: every outside writer PC lies in
     `frame_writers`. Unaccounted PCs are printed with first frame and address;
     disassemble them, decide whether they are frame setup/clears (add a range with
     an evidence comment) or a missed draw span (extend/add a unit);
   - a second machine without sprite units has identical cycles, `native_blocks` and
     (every `--compare-every` frames and at the end) `sync_state_crc()` / `state_crc()`.
4. A mismatch means the span is wrong (too short: state set up before the start PC;
   too long: reads outside the declared start state) or the replay hit something it
   cannot reproduce (device access, interrupts); the report names the first differing
   entry and abort reasons.

The checker runs the native strict path only, so it needs the game's generated
code (`F3_ROM_DIR`). CTest registers `sprite-units` (6000 frames) only when ROMs were
configured and the game declares units.

## Behaviours

`F3RT_SPRITE_BEHAVIOUR(name, unit, description, lambda)` reads the unit's bytes as they
are at unit start and patches fields in the replay's RAM overlay; returning true runs
the game's own code on the patched state and splices the result over the real entries.

Per-behaviour invariants live in `tools/sprite_check.cpp`. For `full-detail` (Command
War, `games/commandw/sprites/behaviours.hpp`) each splice is decoded against its real
entries (matched by sprite identity) and must satisfy:

- Scale: replacement tiles are never larger than the coarse tiles they replace.
- Anchor: replacement and real bounding boxes overlap (measured worst gap over 6000
  attract frames: 0). Equal footprints are not expected: the coarse mip level is
  padded to whole coarse tiles, L0 culls blank tiles, and a stale class (the selector
  at `0x2fed6` runs before projection) lets the real draw clamp at 1:1 while L0 keeps
  growing.
- Shadow: `0x9b32` emits shadow entries first from `$40`, `$42`-`$44`, byte1 bit 7,
  which the behaviour does not touch; the real and replacement entries therefore share
  a bit-identical leading run, and a splice identical to the real entries (no effect)
  fails.
- No aborts, at least one splice, emulated state identical to the units-off machine.

```sh
cmake --build build-check-cw -j10 --target f3rt-sprite-check   # -DF3_GAME=commandw
./build-check-cw/f3rt-sprite-check --frames 6000 --behaviour full-detail
```
