---
source_url: docs/site/developer/runtime/video/producers.md + docs/developer/{VIDEO-HLE,GPU-VIDEO,SPRITE-UNITS}.md
ingested: 2026-10-11
sha256: da3f1507a386b4d47268d4ec3b0432f3833550ba6c84e6a3d3add631bfe26595
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository documentation text (runtime-derived, not hardware); source file deleted after ingest"
---
<!-- excerpt: docs/site/developer/runtime/video/producers.md -->
# Video write logging and fallbacks

**What you will learn:** how `--discovery-log` reports stores from game routines
outside a game's known list, and how the three remaining unsupported features are logged.

All addresses are for the Japan program `landmakrj`. They are not valid for the
World revision `landmakr`.

## Video RAM is the source, not the producers

`GameVideo` does not observe producer routines. At each VBSTART it decodes the
video RAM the FDP reads (see [GameVideo](/developer/runtime/video/game-hle)). A
game-owned routine is therefore only relevant to the discovery log that decides
whether its store PC is *known*.

## Write logging is opt-in

Nothing observes writes unless the executable is started with
`--discovery-log FILE` (no special build; this replaced the `F3RT_VIDEO_WRITE_LOG`
CMake option). The flag creates `Machine::discovery`, and `Machine::write8` then
calls `DiscoveryLog::video_write(pc, address)` for:

- every byte write to 0x600000 to 0x63ffff (graphics RAM);
- every byte write to 0x660000 to 0x66001f (control registers).

`DiscoveryLog` (`runtime/debug/discovery.{hpp,cpp}`) classifies the address into a
`VideoLayer` (`control`, `sprites`, `pf0`..`pf3`, `pf2-alt`, `pf3-alt`, `text`, `lines`;
the Taito F3 layout is the same for every game) and, the first time it sees a `(layer, pc)`, asks the
per-game `video_writer_known(layer, pc)` (**list of known store PCs**). Known PCs are
suppressed; unknown ones get a `NEW video-write` line. Sprite RAM is left to the
emit-unit accounting (`sprite-stray`) when the game declares emit units. See
[the format](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/WORKFLOWS.md#discovery-log).

The log is host-only and never part of machine or snapshot state. A store
from an unknown PC does **not** invalidate anything: the scene is still decoded from
video RAM next frame. Without the flag there is no write observation and the
graphics-write fast path (`direct_bytes`) is unchanged.

::: info
Because the PC is the key, the generated code must expose it. During an
instruction body `cpu->pc` still holds the instruction's own address, so
`Machine::write8` sees the storing instruction's PC. See
[Emission](/developer/recompiler/emission).
:::

## Known store PCs

The per-game `games/<game>/video/` file owns the list through `video_writer_known`
(games without a list report every writer). For `landmakrj`, all of
them are in `games/landmakrj/video/video.cpp`:

| Component | Address range watched | Known producer PCs |
| --- | --- | --- |
| Tiles | 0x610000 to 0x617fff | `tiles_covered_write` |
| Text | 0x61c000 to 0x61ffff | `text_covered_write` (map and glyph writers) |
| Sprites | 0x600000 to 0x60ffff | `sprites_covered_write` ranges 0x41d0 to 0x4380 (init/clear), 0x43b0 to 0x43de (scroll), 0x43e0 to 0x43fe (command), 0x4422 to 0x447e (list terminator), 0x4688 to 0x480a (single/grid), 0x480c to 0x4a36 (scaled), 0xa8f38 to 0xa93a2 (object helpers) |
| Lines | 0x620000 to 0x62ffff and 0x660000 to 0x66003f | `lines_covered_write` ranges, for example 0x00136e to 0x00145e (register uploader), 0x005d30 to 0x005d6c (profile init), 0x09d684 to 0x09d6a0 (board gradient) |

Notes:

- The tile log compares `address - 0x610000` to find the layer (`/ 0x2000`).
- Pivot RAM has no store-PC list; bitmap mode is a rendering fallback, not a
  write-ownership question.

## Fallback kinds

`log_unsupported_video(component, kind, frame)` is always compiled in. It prints
the **first** occurrence of each `(component, kind)` to stderr; `--discovery-log` also records it as `video-fallback`:

```text
game-video: unsupported <component> <kind> frame=N
```

`Impl::fallback(component, reason)` calls it for the only remaining FDP-oracle
fallbacks:

| Kind | Component | Meaning |
| --- | --- | --- |
| `flipped-screen` | `sprites` | Sprite command bit 13. |
| `sprite-trails` | `sprites` | Sprite command bit 1. |
| `bitmap-pivot` | `text` | Pivot control bitmap bits on a visible row. |

The fallback report prints `VIDEO fallback=<reason> frames=N first=F last=L`;
there is no producer PC.

## Timing rules

- **Decode at VBSTART.** `render_frame` builds `VideoRam` and calls `decode` on
  every component before rendering. The one-frame sprite lag is preserved by
  `latch_sprites()`.
- **The discovery log is PC/address only.** It never receives the written value and
  never reads video RAM back to reconstruct a producer.
- **Add a store PC only when it is known.** A missing entry costs one log line
  per run under `--discovery-log`; it does not change the picture.

See [Extending the renderer](/developer/runtime/video/extending) to add a decoder
or a known store-PC range.

Sources: [machine.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/machine.cpp),
[discovery.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/debug/discovery.cpp)
and the per-game `games/landmakrj/video/video.cpp`.

<!-- excerpt: docs/developer/VIDEO-HLE.md lines 157-195 -->
## Unsupported stores and fallbacks

Store logging is opt-in at runtime: `--discovery-log FILE` (see
[WORKFLOWS.md](WORKFLOWS.md#discovery-log)). `runtime/debug/discovery.{hpp,cpp}` classifies
every graphics/control store by address into a `VideoLayer`, asks the per-game
`video_writer_known(layer, pc)` once per new `(layer, pc)` and writes the unknown ones
(first frame, address, hit count). `GameVideo::fallback` reports oracle fallbacks
(component, reason) to the same log. Without the flag `Machine::discovery` is null: graphics
stores keep their fast `direct_bytes` copy. With it, graphics stores take the byte path
(PC attribution) and cost one cached compare each; measured 6000 frames of Command War
unthrottled headless: 10.2 s with the flag, 10.4 s without (noise). A store from an unknown PC never
invalidates anything: the data still comes from video RAM.

Three genuine features remain outside the measured normal-orientation contract
and force the FDP oracle for that frame. `Impl::fallback(component, reason)`
always calls `log_unsupported_video(component, kind, frame)`, which prints the
first occurrence of each `(component, kind)`:

| Fallback | Trigger | Logged component / kind |
| --- | --- | --- |
| `flipped-screen` | sprite command bit 13 | `sprites` / `flipped-screen` |
| `sprite-trails` | sprite command bit 1 | `sprites` / `sprite-trails` |
| `bitmap-pivot` | pivot control `$a0` bits set on a visible row | `text` / `bitmap-pivot` |

The report names the fallback reason, count and first/last affected frame
(`VIDEO fallback=<reason> frames=N first=F last=L`); it is not CPU interpreter
fallback. The default oracle path remains available for every frame.

<!-- excerpt: docs/developer/GPU-VIDEO.md lines 794-829 (producer catalog) -->
### The scaled floor is not the water

The played board's diamond/perspective **ground texture is PF0**, demonstrated
by isolated GPU/CPU-exact PF0 capture at seed 5 frame 6000. PF1 contains the
surrounding architectural backdrop; PF2/PF3 are empty on that frame. The ground
art itself is pre-drawn perspective in ROM tiles, not a per-line perspective
zoom. Its normalized X/Y steps are 256, source-X is row-constant and source-Y
advances by one. At frame 6000 **every isolated layer and the complete 4x image
have zero differences from nearest-enlarged 1x**. There is no extra source
detail to recover there without inventing texture or changing the transform.

The separate animated water/star backdrop during selection **is PF2**:
`$9d66a/$9d72a/$9d7b6`, X-step 2–256, centering source-X, two column-offset
halves and an RLE palette gradient. The old “water/puzzle-board floor” naming
conflated two visually different surfaces. General sampling must handle the
PF0 wave and both valid PF2 ramp halves, not manufacture a transform for the
ordinary unit-scale PF0 floor.

Isolated images: `survey-data/seed5/frame6000.before.state.layers/{pf0,pf1}.png`.
Measured zero-gain and zoomed-sprite baseline:
`/tmp/f3-gpuvideo/general/frozen-survey-gains.log`.

### Producer and boundary catalog

| Producer | Layer/field | Data/formula and boundary |
|---|---|---|
| `$5cd8`, ROM profile `$5d74` | All layer controls, alpha, mosaic, X/Y step | Visible default profile; X step `256-highByte`, Y step `2*lowByte`, cross-PF Y mapping `{0,3,2,1}`. Default/reset is not an effect endpoint. |
| `$136e` | PF/text global scroll | Actual integer/fractional register conversions; frame-level movement is not a line ramp. |
| `$9d72a` | PF2 X zoom/rowscroll | Symmetric about 151/152; first steps at both center rows are 256, then change by 2 per row. Eight-bit wrap is a hard boundary. Centering conversion uses division/remainder at `$9d774..$9d7aa`, not an assumed floating formula. Normalized adjacent source-X steps include ±76/±332/±588 (24.8). |
| `$9d7b6` | PF2/PF3 column offset | Phase `$40790a&127` after task wake; two constant PF2 halves at row 152, plus PF3 rows 0–47 from the uploader spill. These are discrete offsets, not a vertical ramp. |
| `$9ecb0` | PF0 sine rowscroll | Signed-byte phase into actual 32-bit ROM table `$1c84`, low-word negation, shift/swap and phase +2 per line. Source-X wraps at the 1024-texel map period. Interpolate the **packed table samples**, never replace them with analytic sine. |
| `$9d66a`, table `$9d6a8..$9d6d8` | PF2 palette add | 11 held bands: lengths 4/4/4/6/8/8/8/12/16/16/18, offsets 640→0 by 64. Same-pen RGB blending is a separate optional color invention; actual bank discontinuities remain raw. |
| `$91490/$91506/$915d2` | Text/PF3/SP priority/clip/blend | Rectangular selection label/water row blocks; PF3 water rows 176–251, fixed alpha `{5,4,3,4}`. Preserve row-block topology and priority switches. |
| `$91834` | Clip plane 3 | Held edge across rows 176–251, expanding by 6 **per frame**, not a per-line edge ramp. Native half-open clipping stays discrete. |
| `$98dba..$9ad3e`, `$8cfba/$8cfe0` | Alpha, mix and sprite priorities | Uniform row blocks, frame-time fades and saved-alpha restore. Field variation across block borders is not an alpha gradient. |
| `$fe620/$fefe6/$ff0fa` | Ending bitmap/slides | Audited producers explicitly unsupported by semantic lines; exact oracle fallback, no interpolation. Not exercised as a played ending. |

<!-- excerpt: docs/developer/SPRITE-UNITS.md lines 18-53 -->
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
