---
source_url: docs/site/reference/game-config.md
ingested: 2026-10-11
sha256: bee6bd2e0eb40180f52b1e34eb02bc2be4197321b507e467866982eb9457d412
game: landmakrj
rom_sha256: aa050f39f5810c90bc20d1e3ed1c2510b1df067242535ad80aa86b3d174d49bc
captured_with: "repository documentation text (runtime-derived, not hardware); source file deleted after ingest"
---
<!-- excerpt: docs/site/reference/game-config.md -->
# Game config reference

This page describes each key in the per-game TOML file. The recompiler uses it to load ROMs and find code.

## Where the files are

| File | Game | State |
| --- | --- | --- |
| `games/landmakrj/config.toml` | Land Maker Japan 2.01J, set `landmakrj` | Supported player build; selects `all_aligned` coverage and (via `games/landmakrj/video/`) Land Maker-specific VRAM scene decoders. Finite validation only. |
| `games/landmakr/config.toml` | Land Maker World, set `landmakr` | Untested config only; lanes are listed, but there is no `games/landmakr/video/` folder and discovery uses `all_aligned` coverage. |

`F3_GAME` selects `games/SET/config.toml`; default `landmakrj`, with `landmakr`,
`rayforce`, `commandw`, `ridingf` also accepted. The three new titles have native
main/sound bring-up with FDP video and Reference sound, not LM enhanced paths.
Their exact revisions and finite evidence are in [porting](/developer/porting).
You can also pass `--config` directly to the compiler; a config alone is not proof
of a complete port.

The OCaml recompiler `f3recomp` loads the TOML and uses `[discovery]` to
configure code discovery. Validation varies by table. For example, `[cpu]`
rejects unknown keys.

## Value formats

| Format | Where | Rule |
| --- | --- | --- |
| Address | `discovery.entry_points`, `discovery.jump_tables` | A TOML integer or a string containing a decimal or `0x`-prefixed hexadecimal integer. The parser trims whitespace around strings. |
| CRC32 | lane `crc` | A hexadecimal string of 8 digits. The check ignores case. |
| SHA-1 | lane `sha1` | A hexadecimal string of 40 digits. The check ignores case. |

All code addresses must be even (word-aligned) where the table below says so.

## [[exclude]]

Reviewed instruction-start exclusions apply to aligned discovery. Bounds are
guest addresses, start inclusive and end exclusive; both must be even and
inside the selected CPU ROM. Overlap is rejected. Each record requires nonempty
`reason` and `evidence`; `cpu` defaults to `"main"` and may be `"sound"`.

```toml
[[exclude]]
start = 0x11b362
end = 0x1ffffe
reason = "Trailing FF fill, preserving the checksum word"
evidence = "Verified ROM fill; validated seeded native runs"
```

Main ROM addresses start at 0; Japan sound ROM is `0xc00000..0xc80000`.
Data reads and extension words remain available. Vector/config entries
and explicit jump/pointer-table code targets inside an exclusion reject
generation. Apparent direct transfers from independent all-aligned decodes
are reported, not assumed reachable; executing any excluded target fails
at runtime before fallback, even when diagnostic interpretation is enabled.

`uv run f3 rom exclusions` writes proposals and conflicting evidence,
never game configuration. Entropy and missing fetches are not proof of data.
See the repository's [experiment report](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/BINSIZE-EXCLUDE.md).


## Full example

```toml
[game]
name = "Land Maker (Japan)"
id = "landmakrj"
platform = "taito_f3"

[rom]
size = 2097152        # 2 MiB
interleave = 4

[[rom.lanes]]
file = "e61-13.20"
offset = 0
size = 524288
crc = "0af756a2"
sha1 = "2dadac6873f2491ee77703f07f00dde2aa909355"
# ... three more [[rom.lanes]] tables, offsets 1, 2 and 3

[discovery]
coverage = "all_aligned"
entry_points = [0x55c2, 0x56ae]
```

## [game]

This section is optional. It describes the game for people.

| Key | Type | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `name` | string | no | none | Display name. The code does not read it. |
| `id` | string | no | `"unknown"` | Short name. The loader puts it in the error message for a missing lane file. |
| `platform` | string | no | none | Hardware name (`taito_f3`). The code does not read it. |

## [rom]

This section is required. If it is missing or empty, `load_rom()` raises `Config '...' is missing [rom] section.`

| Key | Type | Required | Default | Validation | Meaning |
| --- | --- | --- | --- | --- | --- |
| `size` | integer | yes | none | Must be an integer greater than 0. Else: `must specify positive integer 'size' under [rom]`. | Size of the combined program ROM in bytes. |
| `interleave` | integer | no | `4` | Must be an integer greater than 0, and `size` must be a multiple of it. Else: `ROM size must be a positive multiple of its interleave.` | Number of lane files. The loader writes byte `n` of lane `offset` to image byte `n × interleave + offset`. |
| `lanes` | array of tables | yes | none | Must not be empty. Else: `must define at least one [[rom.lanes]]`. | See below. |

### [[rom.lanes]]

Each `[[rom.lanes]]` table describes one ROM file. The 68EC020 program ROM is 32 bits wide, so four 8-bit chips fill one image. The loader checks the lanes in the order listed.

| Key | Type | Required | Default | Validation | Meaning |
| --- | --- | --- | --- | --- | --- |
| `file` | string | yes | none | Must be set. Else: `Lane definition ... is missing 'file' name.` The file must exist in `--rom-dir`. Else: `FileNotFoundError: Missing ROM lane file ...`. | File name inside the ROM directory. |
| `offset` | integer | no | `0` | Integer from 0 to `interleave - 1`. Else: `has invalid byte offset`. Each offset can appear once. Else: `Duplicate lane offset`. | Byte position of this lane in each group of `interleave` bytes. |
| `size` | integer | no | none | If set, the file length must equal it. Else: `size mismatch`. | Expected file size in bytes. |
| `crc` | string | no | none | If set, the CRC32 of the file must match. Else: `CRC32 mismatch`. | Expected CRC32. |
| `sha1` | string | no | none | If set, the SHA-1 of the file must match. Else: `SHA1 mismatch`. | Expected SHA-1. |

Rules for the whole lane set:

- Each file must have exactly `size / interleave` bytes. Else: `does not fill the configured image`.
- The offsets must cover `0` to `interleave - 1` with no gap. Else: `ROM lane configuration must cover every byte of the image.`
- The loader never replaces a missing file with another ROM set.

In the two Land Maker configs, `size` is `2097152` (2 MiB) and each lane file has `524288` bytes; other titles use their own sizes.

| Lane offset | `landmakrj` file | `landmakr` file |
| --- | --- | --- |
| 0 | `e61-13.20` | `e61-19.20` |
| 1 | `e61-12.19` | `e61-18.19` |
| 2 | `e61-11.18` | `e61-17.18` |
| 3 | `e61-10.17` | `e61-16.17` |

## Additional regions and video metadata

`f3recomp compile-roms` generates runtime metadata from these configs. The sound
compiler consumes the same manifest. Optional region tables are `sprites`, `sprites_hi`, `tiles`, `tiles_hi`, `sound`, `samples`.
Each has explicit `size`, `fill` (0 or 255) and lanes with `file`, `size`, `crc`
(CRC32), `sha1`, `offset`, `stride`, `group`. Every source group of consecutive
bytes is placed at the next destination stride; bounds and hashes are validated.
Riding Fight's absent upper planes use size-zero regions, not fake files.

LM sound lanes can declare validated `short_size`, `short_crc`, `short_sha1`:
validate the short dump, FF-pad to lane size, then validate the full identity.
Command War/Riding Fight use physical `sound.size = 0x40000` with
`mirror_size = 0x80000`, repeating the first bank into the upper half.
RayForce sound is physical/mapped `0x80000`. No `sound.bin` fallback exists.
Main images may be 1 MiB; unpopulated main-map bytes read `0xff`.

`[video]` provides `rotation`, `sprite_lag`, `visible_y`, `visible_height` and
`extend = true` for extended sprite addressing. RayForce America 2.3A
(1994/01/20) uses board 320×224, y 31, ROT90 display 224×320, lag 2.
Command War 0.0J prototype and Riding Fight World 1.0O use 320×224, y 32,
lag 1. Command War retains upstream's imperfect-graphics source limitation;
Riding Fight uses sprite trails.

`extended_alt_maps = true` adds the PF2/PF3 alternate tilemaps (selected per scanline by line RAM
bit `0x200`). `full_resolution_alt_maps = true` (default `false`, requires `extended_alt_maps`;
checked at build time) is a presentation-only option for games such as Command War that draw
distant floor rows from a horizontally half-scaled copy of the main map because F3 horizontal zoom
cannot sample more than one texel per pixel. In expanded output (scale above 1 or a border; GPU and
CPU compositor) such a row samples the main full-resolution map instead whenever the renderer finds
an exact twin of that map row (`alt(x, y) == main((2x + c) mod 1024, y)` for every x and all 16
lines, comparing decoded palette and pen). The native 320-wide frame, `--renderer compare-cpu`, snapshots
and CRCs are unchanged; rows without an exact twin keep the alternate map. The `VIDEO
presented_sprite_frames` stats line reports remapped and fallback alternate-map rows.

Palette precision is dynamic FDA state, not a config key. Latched line RAM
`$6400` bit 14 is active-low 15-bit selection versus 24-bit. The 15-bit format
`RRRRGGGGBBBBRGBx` uses fifth bits 3/2/1 and channel quantization
`nibble * 16 + fifthbit * 8`. Bit 13 is active-low blur. Pen masks come from
tile attributes/sprite commands, not per-game constants. Pinned
[manifest source](https://github.com/mamedev/mame/blob/cfc4760a3be9c5a79846b19b6a573cb38459fa7e/src/mame/taito/taito_f3.cpp)
and [FDA research](https://github.com/y-ack/mame/blob/28e411d4f760df3d55fae070a2f6424f89966a2f/src/mame/taito/tc0630fdp.cpp)
are described in [porting](/developer/porting).

## [[video.emit_units]] (behaviour anchors)

Optional. An emit unit is a span of game code that draws one object or queue record into
sprite RAM. It is only a **behaviour anchor** now: the recompiler's static sprite-group table
(below) supplies grouping and origins, and a declared unit lets a behaviour replay the game's
own native code in a sandbox and splice the result over the real entries, render-only.

```toml
[[video.emit_units]]
name  = "objects"      # C identifier, unique (not all/digest)
start = 0x9a3c         # even PC, or a list of them; each must decode as an instruction
end   = 0x9a6c         # same arity as start; end[i] terminates start[i]; start < end
unit  = "a6"           # d0-d7 / a0-a6: value at start = unit address in work RAM
size  = 0x80           # optional, default 0: bytes behaviours may access at that address
owner = "unit"         # accepted for compatibility; grouping no longer uses it
```

Unit ids are declaration order. `f3recomp sprite-units --config <toml>
--output-dir <dir>` writes, at CMake configure time and without a ROM,
`sprite_units.h` (`F3_SPRITE_UNITS_DIGEST`, `F3_SPRITE_UNIT_COUNT`) and
`sprite_units.hpp` (`f3rt::sprite_units::<name>` `EmitUnit`s, `all`, `digest`,
static_asserts). Games without units get empty arrays. `f3recomp emit` plants
`F3_UNIT_EXIT` then `F3_UNIT_ENTER` at the label of every declared end/start PC, includes
`sprite_units.h` with a digest `#error` guard in each file that has a hook, and
requires each hook PC to be a retained decoded instruction. `[video.frame_writers]`
is retired.

`size` must cover every field a behaviour reads or patches (offsets are compile-time checked
against it). Behaviours live in `games/<id>/sprites/behaviours.hpp`
(`F3RT_SPRITE_BEHAVIOUR`, registered through `F3RT_SPRITE_BEHAVIOURS_HEADER`); the frontend
enables every registered behaviour automatically whenever the game runs natively
(`--translated`).

Select the spans from evidence, not by guesswork: disassemble the draw loop and confirm the
register that holds the record at the start PC. `f3rt-tool sprite-check` reports the
render-only group coverage (`groups`, `group_entries`, `unresolved_entries`) and
verifies that emulated state is identical with sprite groups on and off.

## Sprite groups (generated)

`f3recomp emit` writes `sprite_groups.hpp` into the generated program directory
(`F3_GENERATED_DIR`): `f3rt::sprite_groups::table`, a `SpriteGroupTable`
(`include/f3rt/sprite_group.hpp`) with one `SpriteGroupWrite` per sprite-RAM store PC
(`{pc, base register, lane X/Y/XY, group class, shift, root X/Y field offsets}`) and one
`SpriteGroupReset` per root-field write that starts a new value (spawn/teleport). No
configuration is needed.

At runtime each sprite-RAM write looks its PC up, reads the root fields from work RAM and
derives a render-only group key `(record address, class, per-root generation)` and origin
(`root << shift`, 24.8). Hardware block members (spritecont bit 3) inherit the head entry's
group in the decoder, the same decoder used for rendering. A generation bump (reset write)
changes the key, so a replaced object never reuses the old one. A birth is an event, not a
routine: a reset that also ran for the same root in the immediately preceding frame is a
per-frame recomputation/clamp and does not bump (at most one bump per root per frame), while a
reset after a frame without one still starts a new generation. Anything unresolved leaves
group 0 and the renderer snaps.

### Known ambiguity (commandw multi-part objects)

A key is (record address, class, per-root generation). commandw's multi-part emit site
`0xa0be` emits **two origins for one key in the same frame** (measured over 1500 frames:
49,902 ambiguous sprites, 46,732 of them from `0xa0be` class 1; the record address changes
only 65 times and generations 0 times, and different writer PCs make only 345 unique). The
key deliberately contains no emission ordinal and no writer PC: neither is unique (the same
PC emits both origins, and a multi-part plus single-part pair exists). The renderer therefore
refuses to pair an ambiguous key and snaps those sprites, while the remaining groups pair
with `cross_paired = 0`. The missing discriminator is the draw-list slot or emission pass
(the `0x40885a` list, or the `0x413c00 + k*0x80` fallback), which is an open static-trace
question. The sprite system reports `ambiguity: key_frames / keys / slots / max_*_frame`
every run.

## Compile-time video geometry

`f3recomp compile-roms --game <id> --game-output <path>` also writes
`game_video_config.hpp` into the same `generated_config` directory, from the same
validated `[video]` values as the runtime manifest: `f3rt::game_config::id`,
`video` (a `VideoConfig`), `visible_end`, and `matches(const VideoConfig &)`, which
the renderer calls at startup to reject a loaded set whose geometry differs from
the one compiled in.

## [discovery]

This optional table configures aligned discovery. The recompiler decodes every
even ROM address outside excluded ranges, including overlapping starts. It
collects entry seeds from the 68020 vector table and the configured entry
points. Decoded entries still require selection before they can be dispatched.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `coverage` | string | `"all_aligned"` | The only supported coverage mode. |
| `entry_points` | array of addresses | `[]` | Extra entry seeds. Each address must be even. Addresses outside the ROM are ignored and excluded addresses are rejected. |
| `jump_tables` | array of tables | `[]` | Explicit targets for indirect transfers. The scanner handles sites without a configured table. |

### [[discovery.jump_tables]]

| Key | Type | Required | Meaning |
| --- | --- | --- | --- |
| `address` | address | yes | Even PC of the indirect transfer instruction. |
| `targets` | array of addresses | no | Explicit targets. Each must be even. Targets outside the ROM are dropped. |
| `table` | address | no | Address of a table of 32-bit big-endian pointers. The table must fit in the ROM. |
| `count` | integer | with `table` | Nonnegative number of pointers in the table. |

## [cpu]

This optional table declares which condition code register (CCR) flags ROM
interrupt handlers can observe from interrupted code. The recompiler keeps the
declared flags live at interrupt yield points. Unknown keys are rejected.

| Key | Type | Validation and effect |
| --- | --- | --- |
| `interrupt_observes` | array of strings | Each item is one of `X`, `N`, `Z`, `V`, or `C` (case-insensitive). Omission defaults to an empty set. An explicit list, including `[]`, declares the contract when analysis cannot prove a handler's observations. |
| `interrupt_handlers_trusted` | string | Optional evidence for accepting handlers whose observations cannot be proved. Leading and trailing whitespace is trimmed and the result must be nonempty. |

Generation rejects any proven observation missing from `interrupt_observes`.
An unknown handler requires an explicit declaration or trust evidence by default.

## Recompiler diagnostics

`f3recomp emit` prints a summary to stdout after writing generated code. It
reports entry and tier counts, the interrupt contract and yield points, flag
elision, idle loops, chains, and sprite writer counts. These commands take
`--config <toml> --rom-dir <dir>` and print more focused reports to stdout.

| Command | Report |
| --- | --- |
| `f3recomp coverage` | Counts decoded main ROM instructions whose semantics are unsupported, honoring exclusions. |
| `f3recomp decisions` | TSV with discovery, selection, and sprite decisions. |
| `f3recomp check-irq` | Each ROM interrupt handler's proven flag observations or the location and reason analysis could not prove them. Proven observations outside the declared set are marked undeclared. |

## [input]

Optional input configuration table.

| Key | Type | Default | Meaning |
| --- | --- | --- | --- |
| `dial` | boolean | `false` | Enable paddle / dial spinner counters on analog ports 2 and 3. |
| `six_button` | boolean | `false` | Enable 2-player 6-button layout (extra punch/kick lines on ports 4 and 5). |

## What the config does not contain

- The board memory map and device implementations remain runtime code.
- Sound compiler options remain CLI arguments, but sound ROM placement, hashes, mirroring and exclusions come from the selected TOML.

For how discovery and emit use these keys, read [Discovery](/developer/recompiler/discovery) and [ROM and config](/developer/recompiler/rom-and-config).

## Flicker hold enhancements

```toml
[enhancements]
game_frame_counter = 0x408328

[[enhancements.flicker_hold]]
name = "object-shadow"
group_writer = 0x9c90
anchor_writer = 0x9e9a
period = 2
follow = "self"
```

`game_frame_counter` is the address of the game's 16-bit loop counter.
`group_writer` and `anchor_writer` must resolve to sprite-group stores using
the same base register. `period` is measured in game-loop frames and must
be in 2–255. `follow` selects the held group's own work-RAM root (`self`)
or the matching anchor's root (`anchor`). A held group expires at the
configured period or when the anchor key disappears. Runtime-only builds
without a generated program emit an empty hold table.

The frontend enables the registered behaviours and flicker holds by default.
Use `--list-enhancements` to see their kinds and names. Repeat
`--enhance NAME=on|off` for multiple overrides. Flicker blending applies in
`enhanced` and `compare-gpu`. Compare mode checks an unenhanced GPU
composition against FDP and presents the enhanced frame. The CPU and FDP
compositors retain the emitted flicker.
`sprite-check` reports replacement entries by class and hold statistics,
and compares state CRCs before and after applying the render-only stage.
