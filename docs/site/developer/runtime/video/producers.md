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

`DiscoveryLog` (`runtime/discovery_log.{hpp,cpp}`) classifies the address into a
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
[discovery_log.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/discovery_log.cpp)
and the per-game `games/landmakrj/video/video.cpp`.