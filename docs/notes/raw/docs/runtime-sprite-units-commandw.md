---
source_url: docs/developer/SPRITE-UNITS.md
ingested: 2026-10-11
sha256: 52374d7307345c11fe75ebb71c43d42a62cc14d4f01024293bde784512ac54a3
game: commandw
rom_sha256: 09941ebd5dea6b128c3375560dbe3942e55bd0942e50e791c062d88a8cc5dab1
captured_with: "repository documentation text (runtime-derived, not hardware); source file deleted after ingest"
---
<!-- excerpt: docs/developer/SPRITE-UNITS.md lines 55-103 -->
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
