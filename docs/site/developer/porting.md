# Porting another Taito F3 game

f3-recomp currently has an exercised **Land Maker Japan 2.01J (`landmakrj`)** implementation, not an all-game F3 compatibility layer. Its 68020 compiler and several board-level components are reusable, but the ROM loader, automatic build, native sound identity and game-data video producers contain Land Maker assumptions. A new TOML file alone is not a complete port.

This audit distinguishes three things:

- **Configured:** source contains a ROM definition or implementation path.
- **Exercised:** Land Maker Japan has execution/reference-comparison evidence; this is not a claim about every possible playthrough.
- **Physically verified:** no physical TC0630FDP verification is established here. Video and sound-device implementations are MAME-derived; matching reference output does not establish hardware accuracy. Native sound executes compiled sound-CPU instructions through emulated devices; it is **not HLE**.

## Support and ROM identities

| Game | Configuration and runtime loading | Generated/native integration | Evidence boundary |
| --- | --- | --- | --- |
| Land Maker Japan 2.01J, `landmakrj` | Main lanes, hashes, all-aligned discovery, video hooks and main/sound exclusions in [`games/landmakrj/config.toml`](https://github.com/ansxor/f3-recomp/blob/main/games/landmakrj/config.toml); accepted by `RomSet::load` | Automatic CMake main/sound generation, `landmakr` target and frozen execution profile | Only exercised game/revision. See [validation evidence](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/VALIDATION.md); do not generalize it to World or other F3 games. |
| Land Maker World, `landmakr` | Main lanes/hashes and recursive discovery settings in [`games/landmakr/config.toml`](https://github.com/ansxor/f3-recomp/blob/main/games/landmakr/config.toml); runtime accepts its main lanes and shared LM assets | No World profile or Japan producer hooks; automatic CMake generation still selects Japan; `landmakr` executable rejects World | Config-only, untested. The config explicitly says World lanes were not supplied. Runtime acceptance is not observed boot/play support. |
| Other F3 games | No configuration and rejected by `RomSet::load` | No build selection, profile or game-specific producers | Unsupported; no compatibility claim. |

### Main program sets

Both configured images are 2 MiB, formed from four 512 KiB byte lanes at stride four. The config loader verifies lane size, CRC32 and SHA-1; the runtime loader verifies size and CRC32 using separately hard-coded tables.

| Lane offset | Japan filename / CRC32 | World filename / CRC32 |
| --- | --- | --- |
| 0 | `e61-13.20` / `0af756a2` | `e61-19.20` / `f92eccd0` |
| 1 | `e61-12.19` / `636b3df9` | `e61-18.19` / `5a26c9e0` |
| 2 | `e61-11.18` / `279a0ee4` | `e61-17.18` / `710776a8` |
| 3 | `e61-10.17` / `daabf2b2` | `e61-16.17` / `b073cda9` |

Sources: the two game configs; [`recomp/discovery.py`, `load_rom`](https://github.com/ansxor/f3-recomp/blob/main/recomp/discovery.py); [`runtime/rom.cpp`, `RomSet::load`, `chip`, `lane`](https://github.com/ansxor/f3-recomp/blob/main/runtime/rom.cpp). Config SHA-1 values are authoritative rather than duplicated here.

### Shared Land Maker graphics and sound sets

`RomSet::load` uses these same asset definitions for Japan and World. They are not obtained from `[rom]` in the TOML files.

| Region | Chips / CRC32 | Runtime arrangement |
| --- | --- | --- |
| Sprite low planes | `e61-03.12` / `e8abfc46`, `e61-02.08` / `1dc4a164` | Two 2 MiB chips, byte-interleaved into 4 MiB |
| Sprite high planes | `e61-01.04` / `6cdd8311` | 2 MiB |
| Tile low planes | `e61-09.47` / `6ba29987`, `e61-08.45` / `76c98e14` | Two 2 MiB chips, two-byte groups at stride four into 4 MiB |
| Tile high planes | `e61-07.43` / `4a57965d` | 2 MiB |
| Sound program | `e61-14.32` / `18961bbb`, `e61-15.33` / `2c64557a` | Two 256 KiB lanes into 512 KiB; 128 KiB dumps with CRCs `b905f4a7` / `87909869` are accepted and FF-padded |
| Samples | `e61-04.38` / `c27aec0c`, `e61-05.39` / `83920d9d`, `e61-06.40` / `2e717bfe` | Three 2 MiB chips in even byte lanes at `0x400000`, `0x800000`, `0xc00000` of a zero-filled 16 MiB region |

The mapped sound program is `0xc00000..0xc80000`, excluding the unused prefix of MAME's sound region. [`tools/compile_sound.py`, `load_sound_rom`](https://github.com/ansxor/f3-recomp/blob/main/tools/compile_sound.py) independently pins the two sound chip names, padding and combined image CRC32 `5a7e9117`. [`runtime/sound_native.cpp`, `SoundNative::SoundNative`](https://github.com/ansxor/f3-recomp/blob/main/runtime/sound_native.cpp) checks that same combined CRC. Passing another game's config to the sound compiler changes exclusions, not chip selection or this identity check.

## Reusable pieces, with limits

| Piece | Reusable responsibility | What it does not establish |
| --- | --- | --- |
| [`recomp/discovery.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/discovery.py), `discover` | Capstone 68020 decoding, vector seeds, configured entries/hooks/jump tables, recursive or all-aligned coverage, instruction-start exclusions | Task-trap/callback/script heuristics embody software conventions. Successful decoding, including overlapping aligned candidates in data, does not prove reachability or supported lowering. |
| [`recomp/emitter.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/emitter.py), `lower`; [`recomp/cpu_ops.h`](https://github.com/ansxor/f3-recomp/blob/main/recomp/cpu_ops.h); [`recomp/generate.py`](https://github.com/ansxor/f3-recomp/blob/main/recomp/generate.py), `generate` | Instruction semantics, generated blocks, dispatch tables, exception entries, configurable observation hooks and ABI guards | Coverage/lowering inventories are not proof that a different game's reachable instructions, exceptions and dynamic targets run correctly. |
| [`include/f3rt/cpu_abi.h`](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/cpu_abi.h), `f3_cpu`, `f3_block`, `f3_register_blocks`, `f3_dispatch`; [`runtime/cpu_abi.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/cpu_abi.cpp) | Versioned C contract for CPU state, bus access, exceptions and event-boundary dispatch | ABI version 3 is an interface contract, not a game-support guarantee. Its main-CPU cycle clock is explicitly a scheduling clock, not exact timing. |
| [`include/f3rt/machine.hpp`](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/machine.hpp); [`runtime/machine.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/machine.cpp), `Machine`, `advance_to`, `boundary` | Main bus, RAM, palette/graphics/control memory, inputs, shared sound RAM, EEPROM, watchdog, frame/IRQ scheduling, save/load state | Constructor requires a 2 MiB main image. Map, clocks, IRQ behavior, 320×232 output and peripheral coverage must be checked for a new title; generic naming does not establish all-board correctness. |
| [`runtime/video.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/video.cpp), `Video::render_frame`, `decode_roms`; [`include/f3rt/video.hpp`](https://github.com/ansxor/f3-recomp/blob/main/include/f3rt/video.hpp) | MAME-derived FDP-memory renderer: sprites, playfields, text/pivot, line effects, clipping and mixing | No Japan producer PCs are needed for this path, but ROM decode currently requires at least 4 MiB low + 2 MiB high planes for both sprites and tiles, decoding a fixed 32768 16×16 6-bpp tiles per kind. Other region sizes/layouts/features need evidence and possibly changes. |
| [`runtime/audio.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/audio.cpp), `Audio::Impl`; [`runtime/third_party/audio/`](https://github.com/ansxor/f3-recomp/tree/main/runtime/third_party/audio) | Sound bus and MAME-derived ES5505, ES5510, MC68681 DUART and MB87078 devices; shared CPU-runner interface | Device reuse is not proof of another driver's timing, banking, DSP or sample layout. Native and oracle CPU execution use these devices, not a high-level music replacement. |
| [`runtime/interpreter.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/interpreter.cpp), `Interpreter`; [`runtime/eeprom.hpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/eeprom.hpp) | Musashi diagnostic/reference CPU execution and EEPROM model | Reference execution is useful for bring-up, not an acceptable silent fallback in a strict-native release. |

For example, `Machine::read8` models ROM at `0x000000..0x1fffff`, mirrored work RAM at `0x400000..0x43ffff`, palette at `0x440000`, inputs at `0x4a0000`, graphics at `0x600000..0x63ffff` and shared sound RAM at `0xc00000`. Unmapped reads return MAME's `0xff`; the source explicitly avoids claiming a physical-board mirror model. These are concrete implementation limits to compare against a new game's requirements.

## Where Land Maker is baked in

### Build and generated program selection

[`CMakeLists.txt`](https://github.com/ansxor/f3-recomp/blob/main/CMakeLists.txt) labels `F3_ROM_DIR`, `F3_GENERATED_DIR` and `F3_SOUND_GENERATED_DIR` as Land Maker inputs. Automatic generation explicitly uses `games/landmakrj/config.toml`, outputs into `generated/landmakrj` and `generated/sound-landmakrj`, and selects `profiles/landmakrj.profile` by default. The application target is named `landmakr` and defines `F3RT_LANDMAKR`.

The main compiler can accept a different config directly, and CMake can link a pre-generated directory. Neither facility updates the runtime ROM tables, executable gates, sound identity or producer decoders. `f3_generated_register` registers generated blocks/exclusions; its implementation does not compare the loaded main image against a generated ROM digest. Therefore selecting a runtime set and selecting generated code must remain consistent; `--set` is not a native game selector.

### Observation hooks and game-data scene decoding

Japan's `[[hooks]]` table calls `f3_landmakr_video_hook` at specific program PCs. `generate` flushes pending condition flags and invokes the hook before the original native instruction; this is observation, not replacement of the game's instructions. [`runtime/game_video.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_video.cpp), `f3_landmakr_video_hook` and `GameVideo::observe`, distribute observations to four producers.

[`runtime/game_scene.hpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_scene.hpp), `GameMemory`, permits producer reads from program ROM and work RAM, not FDP RAM. The scene structures and compositor are reusable representations; the producers' calling conventions, tables and write whitelists are Japan-specific:

| Producer | Representative dependencies (not the complete hook list) |
| --- | --- |
| [`GameTiles::observe`, `observe_write`](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_tiles.cpp) | Tile-block helper PCs `0x55c2`, `0x5614`, `0x56ae`; per-layer clears `0x5a22..0x5af4`; selection strips `0x9ec4e`; stack/register descriptor conventions; expected writes into `0x610000..0x617fff` |
| [`GameText::observe`, `observe_write`](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_text.cpp) | Text helpers at `0x56e6`, `0x5726`, `0x5768`, clear at `0x59bc`; glyph data/uploads into `0x61e000..0x61ffff`; hard-coded solid glyph behavior at `0x8de56`, `0x8e9c6`, `0x9b530`, `0xa1170` |
| [`GameSprites::is_covered_write`, `parse_single`, `parse_grid`](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_sprites.cpp) | Initialization range `0x41d0..0x4380`, object helpers including `0xa8f38..0xa93a2`, object descriptor/register formats, modeled sprite writes into `0x600000..0x60ffff` |
| [`GameLines::observe`, `load_default_profile`, `is_covered_write`](https://github.com/ansxor/f3-recomp/blob/main/runtime/game_lines.cpp) | Display uploader `0x136e`, boot uploader `0x10044`, default profile ROM table `0x5d74`, flip byte at work RAM `0x40013e`; explicit water, attract, gameboard and selection effect PCs; line/control write whitelist |

The full Japan config and producer switches/range tables are the authoritative inventory. Relocating these addresses is not enough for another game: it may use different helper semantics, descriptor formats, timing or effect producers, even on the same board.

`GameVideo::render` checks producer support and can use the FDP reference image when a component is unsupported. It also explicitly falls back for flipped-screen, sprite-trails and bitmap-pivot scenes; the modeled game-data path is not a complete substitute for every FDP feature. That video fallback is separate from main-CPU interpreter fallback. A visually plausible frame alone does not prove that new game-data producers worked; inspect component support and compare individual layers and composites. Producer state, sprite latches and presentation state also participate in snapshots.

### Frontend and sound gates

[`runtime/frontend.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/frontend.cpp) defaults `--set` to Japan. The `F3RT_LANDMAKR` executable rejects any other set and defaults strict-native execution to game-data video. Explicit `--video game|compare` requires **strict-native `landmakrj`**; FDP video cannot use the expanded/game-data presentation enhancements. CPU video backend and interpolation off remain defaults; GPU presentation is opt-in and requires game/compare mode.

Builds with generated sound default to native sound unless explicitly selecting `--sound-driver oracle`. Both remain emulated-device paths. Beyond the pinned loader CRC, native sound assumes the fixed `0xc00000` base and `0x80000` size in [`runtime/sound_native.hpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/sound_native.hpp). `compile_sound_rom` uses a 68000 decoder, instruction-boundary timing from `68000_cycles.csv`, aligned candidate entries and explicit unsupported handlers. New drivers need their own lowering/timing evidence, not just removal of the CRC guard.

`SoundNative::trace_sound` also recognizes LM driver PCs `0xc130f0`, `0xc140e4` and `0xc17632` for direct-note and voice context. Such diagnostic semantic labels must not be treated as generic F3 sound-driver information.

## Profiles, exclusions and netplay identity

The sole committed profile, [`profiles/landmakrj.profile`](https://github.com/ansxor/f3-recomp/blob/main/profiles/landmakrj.profile), identifies the main image as CRC32 `15a59a08`, base `0`, size `0x200000`, and sound as `5a7e9117`, base `0xc00000`, size `0x80000`. [`recomp/block_profile.py`, `load_hot`](https://github.com/ansxor/f3-recomp/blob/main/recomp/block_profile.py) matches ROM identity/bounds and checks exclusions. Reusing Japan's profile for another main image is not a supported optimization strategy.

- **Tiers:** retain the full post-exclusion candidate coverage, compiling profiled hot entries with `-O2` and cold entries with `-Oz`/`-Os`. This is the automatic Japan default.
- **Slim:** explicitly opt-in removal of cold main/sound entries; cold hits abort. It is not a general-play support baseline, and sampled execution cannot establish that omitted entries are unreachable.
- **Exclusions:** remove instruction starts, not ROM bytes available to data reads. Excluded execution fails even with diagnostic interpreter fallback enabled. Japan's ranges are evidence for that image only.

| CPU | Japan half-open excluded intervals | Config rationale |
| --- | --- | --- |
| Main | `[0x007030,0x010000)`, `[0x11b362,0x1ffffe)` | Interior/trailing FF fill; preserves final checksum word |
| Main | `[0x020000,0x088000)` | Graphics/script-table data bank |
| Sound | `[0xc1e45a,0xc20000)`, `[0xc39a30,0xc80000)` | Interior/trailing FF fill, including unpopulated chip halves |
| Sound | `[0xc20000,0xc39a30)` | Sound sequence/header data bank |

See [`parse_exclusions`](https://github.com/ansxor/f3-recomp/blob/main/recomp/discovery.py), the Japan config's `[[exclude]]` records, and [exclusion evidence](https://github.com/ansxor/f3-recomp/blob/main/docs/developer/BINSIZE-EXCLUDE.md). Absence from a sampled profile is not universal proof of data-only use.

[`runtime/netplay.cpp`, `machine_identity`](https://github.com/ansxor/f3-recomp/blob/main/runtime/netplay.cpp) hashes all seven loaded ROM regions, build identity and canonical audio/video state format. EEPROM, initial local state, requested delay and presentation are not identity fields; the host supplies canonical match state. CMake's build identity includes runtime/header/compiler/config inputs, generated main/sound code and profile mode/optimization settings. This prevents treating a matching game name as sufficient peer compatibility; it does not prove determinism by itself.

Current frontend netplay requires strict-native main execution, compatible canonical video/audio formats and no sound tracing. Independent EEPROM histories and presentation geometry are supported. The versus-entry/exit predicates, host preparation and two-slot input mapping are Japan 2.01J contracts that need review for another title. Game-data video and the HLE audio decoder also remain ROM-specific; the overlay and shared transport do not make another F3 game supported.

## Concrete bring-up work for another game

### First: conservative FDP-memory rendering

1. **Identify the exact set/revision.** Supply legally obtained main, graphics, sound and sample chips; record trusted lengths/hashes, interleave/group/region placement and any padding. Add a main config and runtime region definition; do not assume LM asset geometry or sound bank placement.
2. **Select that config end-to-end.** Make the build/executable load the matching ROM and generated main code. Do not rename a Japan output directory or bypass a gate while retaining Japan code/profile. Begin without copied LM hooks, exclusions or hot-set assumptions.
3. **Establish instruction coverage.** Review vectors, indirect targets, task/callback conventions, decoder rejections and lowering reports. Exercise strict native mode with zero main fallback instructions. Add semantics or discovery metadata for demonstrated gaps, rather than quietly interpreting them.
4. **Check board/device requirements.** Confirm memory map, inputs, EEPROM, reset/watchdog, IRQ/frame timing, graphics layout and audio sample banking. Adapt the fixed renderer decode geometry where necessary. Use `--video fdp` initially: this renders the game's ordinary device-memory writes without LM producer hooks.
5. **Bring up sound independently.** The oracle CPU runner can diagnose the driver through existing emulated devices. To ship native sound, configure the new sound ROM identity/loading/base/size and generated dispatch contract, revise exclusions only with evidence, and verify 68000 instruction timing, exceptions, reset/IRQ handling and device writes. Remove or replace LM-specific trace semantics for that driver.
6. **Report support at the level exercised.** A boot screen does not establish attract mode, service checks, gameplay, sound or full-game coverage. World is also a separate revision requiring this discipline, even though LM asset tables are shared.

### Then, optionally: game-data rendering and enhanced presentation

Reverse-engineer the new game's tile, text/glyph, sprite and line-effect producers. Implement its own hook table and decoders, or demonstrate exact compatibility with existing LM producer semantics. Include writer coverage/invalidation, latch ordering and save-state round trips. Compare scene layers, palette/flags and composites to the FDP renderer across transitions and effects; reference-to-reference comparisons still need external MAME evidence for the conservative path.

Only after this should frontend game/compare gates, expanded presentation and GPU eligibility be extended. Game-data rendering is optional for conservative offline bring-up, but current frontend netplay depends on it; extending netplay requires explicit integration and deterministic rollback/input/audio evidence, not simply exposing the connection flags.

### Evidence needed before claiming support

- Boot and service/ROM tests; attract/demo, inputs, coin/start, EEPROM lifecycle and representative gameplay/transitions under the exact revision.
- Strict-native main execution with no interpreter fallbacks; exercised unsupported/excluded/cold-hit diagnostics where relevant. Keep post-exclusion full coverage as the general-play baseline.
- Deterministic main state and native/oracle sound comparisons, including reset/IRQ timing and device traces; compare resulting audio, not just instruction counts. Cover music, effects and DSP/bank behavior used by the title.
- FDP frame/layer captures against a pinned MAME reference, with documented inputs and duration. If game-data video is added, compare both paths and verify unsupported-producer behavior rather than mistaking FDP fallback for successful producer coverage.
- Snapshot restoration and replay/rollback, including sound, producer latches and EEPROM. For netplay, matching ROM/build/canonical-format identity, validated entry/exit and handoff plus sustained two-peer synchronization under delay/loss scenarios and the new game's input mapping.
- Physical-board measurements/captures **only if claiming hardware accuracy**. MAME output agreement alone does not justify that wording.

Existing test/tool locations are starting points, not reusable proof for a new game: [`runtime/check.cpp`](https://github.com/ansxor/f3-recomp/blob/main/runtime/check.cpp), [`tools/gameplay_regression.cpp`](https://github.com/ansxor/f3-recomp/blob/main/tools/gameplay_regression.cpp), [`tools/gpu_video_regression.cpp`](https://github.com/ansxor/f3-recomp/blob/main/tools/gpu_video_regression.cpp), [`tools/sound_extract.cpp`](https://github.com/ansxor/f3-recomp/blob/main/tools/sound_extract.cpp) and [`tools/netplay_oracle.cpp`](https://github.com/ansxor/f3-recomp/blob/main/tools/netplay_oracle.cpp). Inspect their game assumptions before extending them. Retain reproducible validation evidence outside user-facing support claims.

## Bounded future refactor candidates

These are portability seams, not implemented features or prerequisites to redesign the whole runtime:

1. **One per-game ROM manifest:** describe all regions, lane/group placement, accepted dump variants and hashes; consume it at build and runtime instead of maintaining main TOML plus LM C++/Python asset tables.
2. **Explicit generated-game selection and binding:** select the main config, sound config, output directories, executable identity and profile together; validate generated main/sound image identity against loaded ROMs.
3. **Sound compiler/runtime parameters:** move chip names, expected identity and mapped bounds into per-game metadata, retaining strict validation and unsupported-instruction failures.
4. **Game-video adapter boundary:** separate LM hook PCs, writer ranges, data/calling conventions and effect tables from shared scene/compositor code. Do not label existing LM producers generic by moving files alone.
5. **Graphics region geometry:** parameterize only the decode counts/layouts needed by an evidenced second title; keep current LM layout explicit.
6. **Revision-scoped profiles and exclusions:** select them alongside image identity, preserving their evidence and the distinction between full-coverage tiers and code-removing slim builds.
7. **Per-title frontend/input eligibility:** express strict-native, game-data and netplay requirements without implying support from a CLI set name. Keep deterministic identity and conservative defaults intact.

## Credits and licensing constraints

[`runtime/LICENSES.txt`](https://github.com/ansxor/f3-recomp/blob/main/runtime/LICENSES.txt) credits individually BSD-3-Clause MAME adaptations, pinned to revision `cfc4760a3be9c5a79846b19b6a573cb38459fa7e`: Bryan McPhail, ywy and 12Me21 for video; Bryan McPhail, Aaron Giles, R. Belmont and Philip Bennett for audio; and the listed ES5505/ES5510/DUART/volume authors. Preserve source notices and reproduce required notices in binary distributions. This does not mean the MAME framework or GPL-only source is linked into f3rt, nor does it permit indiscriminate copying from other MAME files when adding a game.

Musashi's Karl Stenerud permission notice is MIT-style, but its SoftFloat Release 2b dependency has separate non-MIT/BSD permissive terms, including responsibility/indemnification conditions and mandatory source notices. SDL3 is a system dependency under the zlib license. Consult the retained component notices before distributing a port. No repository-wide license grant is supplied; these dependency notices apply to their respective components, not automatically to all project code.

Game ROMs, decoded assets and captured media have separate rights. Hash tables/configs are not a license to redistribute those assets. Keep ROMs and generated game artifacts out of published source/site content.
