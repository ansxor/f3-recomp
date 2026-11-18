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
For `full-detail` (Command War, `games/commandw/sprites/behaviours.hpp`) the splice is
decoded against its real entries and must keep the same footprint; emulated state must be
identical to the groups-off machine and at least one splice must be produced.

## Multi-emission ambiguity

The `objects` record is not unique per drawn sprite in a frame: the multi-part emit site
`0xa0be` produces two origins for the same (record, class, generation) (49,902 ambiguous
sprites over 1500 frames, 46,732 from `0xa0be` class 1; writer PCs explain only 345, and the
record address/generation are stable). Sprite groups therefore leave such keys ambiguous and
the renderer snaps them; the draw-list slot or emission pass (`0x40885a` list /
`0x413c00 + k*0x80` fallback) is the candidate discriminator and is still an open static-trace
question. See `runtime/sprites/groups.hpp` next to `pack_key`.

## Shadows (game-side)

**Command War** (`object_shadow`, unit `objects`), found by disassembly:

- `0x680a  addq.w #1,-$7cd8(a5)` increments a 16-bit counter once per game loop iteration
  (a5 = `0x410000`: word `0x408328`, parity byte `0x408329`).
- `0x9b32` compiles one object. `btst #7,$1(a6)` (record byte 1 bit 7, the object has a
  shadow) else jump to `0x9d32`; `btst #0,-$7cd7(a5)` (counter parity) else jump to `0x9d32`.
  Otherwise `0x9b4c..0x9d30` emits the shadow entries (inputs `$40`, `$42`-`$44`) and `0x9d32`
  the object body. So the shadow exists only on odd counter parity. The game loop itself often
  runs every other frame, so the original shadow is on for two frames and off for two.
- The calls inside the span (`0xa30e`/`0xa31e` tables) only adjust `a0`/`a2`.

Sprite-group class 2 marks the shadow coordinate store at `0x9c90`. Class 1
marks body coordinate stores at `0x9e9a` and `0xa0be`. The full-detail replay
patches the entire object and resolves group metadata for each replacement
entry using its own store PC and the sandbox's work RAM.

The GPU's `object-shadow` enhancement holds the shadow through the skipped
game-loop frame and uses a 1/2 linear-light blend on both emitted and held
frames. The matching body key confirms the object is still present. For 576
complete gaps in the native attract trace, the self root sampled on the final
absent frame matched the first reappearing shadow root in 576 cases (mean L1
error 0 pixels). Translating by the body-root delta matched 152 (mean L1
error 1.87 pixels). The configured self root is at record offsets
`+0x44/+0x46`. `--enhance object-shadow=off` restores the alternating GPU display.
