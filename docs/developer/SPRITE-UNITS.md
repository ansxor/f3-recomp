# Emit-unit replay and sprite behaviours

Render-only machinery (nothing here is serialized or part of `Machine` state):

- `[[video.emit_units]]` / `[video.frame_writers]` in `games/<id>/config.toml`
  ([syntax](../site/reference/game-config.md#videoemit_units-and-videoframe_writers)).
- `runtime/sprites/units.{hpp,cpp}`: identity tracking, synchronous sandbox replay
  (copy-on-write RAM overlay, no emulated side effects), splices, check mode.
- `runtime/sprites/behaviour.hpp`, `games/<id>/sprites/behaviours.hpp`:
  per-game patches applied to the replay.

Units and behaviours are always on: the frontend creates `SpriteUnits` whenever the
game declares units and runs natively (`--translated`; the interpreter has no unit
hooks), and enables every registered behaviour. There are no flags. They never change
cycles, native block counts or `Machine::state_crc()`. The
checker tools keep their own on/off switches for parity proofs.

## Workflow: add or change a unit

1. Find the loop that draws one object/record and note the span PCs and the register
   holding the unit address (see Command War and Land Maker configs for annotated
   examples).
2. Configure (`cmake -S . -B <dir>`; regenerates the hooks) and run the checker:

   ```sh
   cmake --build build -j10 --target f3rt-tool
   ./build/f3rt-tool sprite-check --rom-dir ../roms/landmakr --frames 6000
   ```

   `--set` defaults to the compiled game; `--frames N` (default 6000);
   `--compare-every N` (default 60) verifies that identical inputs advance state identically;
   `--seed N` / `--no-inputs` control the coin/start/button-mashing schedule shared
   with `f3rt-tool gameplay`; `--behaviour NAME` is repeatable.
3. Read the report. It must end in `PASS` (exit code 0):
   - every invocation `matched` (unpatched replay equals the real writes bit for
     bit), no aborts;
   - `sprite writes outside units ... (unaccounted 0)`: every outside writer PC lies in
     `frame_writers`. Unaccounted PCs are printed with first frame and address;
     disassemble them, decide whether they are frame setup/clears (add a range with
     an evidence comment) or a missed draw span (extend/add a unit);
   - a second machine without sprite units has identical cycles, `native_blocks` and
     (every `--compare-every` frames and at the end) `state_crc()`.
4. A mismatch means the span is wrong (too short: state set up before the start PC;
   too long: reads outside the declared start state) or the replay hit something it
   cannot reproduce (device access, interrupts); the report names the first differing
   entry and abort reasons.

Humans can get the unaccounted-writer list from ordinary play with
`--discovery-log FILE` (category `sprite-stray`; see [WORKFLOWS.md](WORKFLOWS.md#discovery-log)).

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
  The `Shadow` invariant above holds with flicker shadows on too: a splice whose
  replacement equals the real entries and carries tagged shadow entries is a flicker splice,
  not a no-effect patch, and footprints compare the object body only (tagged entries
  excluded).

## Flicker shadows

Some games fake translucent shadows by emitting the shadow sprites on alternate frames only.
On a display that does not run at the game's ~58.9 Hz that shows the shadow for one or two
presented frames unevenly. A `FlickerShadow` source (`F3RT_FLICKER_SHADOW` in
`runtime/sprites/behaviour.hpp`, declared in `games/<id>/sprites/behaviours.hpp`
next to `behaviours`) names the game's shadow emit path inside an emit unit; like units and
behaviours it is always on (no flag) but **only for the GPU backend with game video**
(`--renderer enhanced`, or developer `compare-gpu`; CPU and FDP output are untouched and keep the game's own
alternate-frame flicker), render-only, and never changes
cycles, native blocks or `state_crc()`.

**Command War** (`object_shadow`, unit `objects`), found by disassembly:

- `0x680a  addq.w #1,-$7cd8(a5)` increments a 16-bit counter once per game loop iteration
  (a5 = `0x410000`: word `0x408328`, parity byte `0x408329`).
- `0x9b32` compiles one object. `btst #7,$1(a6)` (record byte 1 bit 7, the object has a
  shadow) else jump to `0x9d32`; `btst #0,-$7cd7(a5)` (counter parity) else jump to `0x9d32`.
  Otherwise `0x9b4c..0x9d30` emits the shadow entries (inputs `$40`, `$42`-`$44`) and `0x9d32`
  the object body. So the shadow exists only on odd counter parity. The game loop itself often
  runs every other frame, so the original shadow is on for two frames and off for two.
- The calls inside the span (`0xa30e`/`0xa31e` tables) only adjust `a0`/`a2`.

How it works:

1. At unit start the replay (the same sandbox as behaviours) additionally sets the gate bits
   (`gate_register + gate_offset`, `gate_mask`) in its RAM overlay for units where `applies`
   holds, so the shadow exists on every game frame. Entries written while the writer PC lies
   in `emit` are tagged `sprite_flag_shadow` (replay: `SpriteSplice::flags`; real entries:
   `SpritePresentation::flags`, from the real writes' PC). Detection is by writer PC, not
   by position or tile.
2. Decoders carry the tag (`DecodedSpriteEntry::flags`, `SceneSprite::shadow`). It is
   render-only: the canonical list and serialized state never see it.
3. `GpuVideo` shows the shadow as a steady blend, not an alternation. Window presents draw the
   sprite pass twice: plane A holds every sprite, plane B the same list with tagged sprites
   culled (`F3_MASK_HIDE_SHADOW`). The scene shader composites both (`shade_mixed`,
   `F3_MASK_BLEND_SHADOW`) and, where they differ, outputs the per-channel average in linear
   light (sRGB decode, mean, re-encode): exactly the time average of the frames with and
   without the shadow, because the eye integrates emitted light. Overlapping shadows
   cannot double-darken (plane A is opaque overdraw, as in the "on" frame). Readback and reference
   draws (`output` non-empty) never blend and keep plane A. Nothing depends on the display
   refresh any more: no display-rate redraw, no presented-frame parity; only `--motion-interp`
   uses the display-link pacing.
4. CPU and FDP output are unchanged (the game's own flicker), by design: the feature is the GPU
   presenter's. The CPU reference raster draws tagged shadows, i.e. the "visible" phase, but
   shadows are never enabled for it.

`f3rt-tool sprite-check` enables the source (switch: `--no-flicker-shadows`) and checks inside
`SpriteUnits::check_flicker`: tagged replay entries equal the real shadow-path entries when the
game's gate was set, none were real when it was clear, tagged entries lead the run, and
forcing the gate adds nothing but the shadow (untagged remainder equals the real entries). Per
frame it also requires tagged entries in the presentation, detection in attract mode (before
the first coin) and on frames of both game parities. `--shadow-trace N` prints per-frame
counts:

```sh
./build-commandw/f3rt-tool sprite-check --frames 6000 --behaviour full-detail \
    --shadow-trace 12
```

```sh
cmake --build build-check-cw -j10 --target f3rt-tool   # -DF3_GAME=commandw
./build-check-cw/f3rt-tool sprite-check --frames 6000 --behaviour full-detail
```
