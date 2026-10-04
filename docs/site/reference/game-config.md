# Game config reference

This page describes each key in the per-game TOML file. The recompiler uses it to load ROMs, find code and insert hooks.

**What you will learn:**

- Which sections and keys the file can contain.
- The type, default and validation rule of each key.
- Which error message each rule gives.
- How the two shipped configs differ.

## Where the files are

| File | Game | State |
| --- | --- | --- |
| `games/landmakrj/config.toml` | Land Maker (Japan), set `landmakrj` | The build uses this file. It selects `all_aligned` coverage and defines 69 video hooks. |
| `games/landmakr/config.toml` | Land Maker (World), set `landmakr` | Config only. Lanes are listed, but the file has no Japan-specific addresses and uses the default `recursive` coverage. |

The top-level `CMakeLists.txt` uses `games/landmakrj/config.toml` only. You pass a config file to the recompiler with `--config`. See [CLI reference](/reference/cli#python3-m-recomp).

The loader is `load_rom()` in `recomp/discovery.py`. The discovery step (`discover()`) reads `[discovery]` and `[[hooks]]`. The emit step (`generate()` in `recomp/generate.py`) reads `[[hooks]]` and `[discovery] coverage` again.

::: info
The loader does not check unknown keys. A misspelled key is silently ignored. Check the spelling of every key.
:::

## Value formats

| Format | Where | Rule |
| --- | --- | --- |
| Address | `entry_points`, `jump_tables`, `inline_string_helpers`, actor script addresses, hook `address` | An integer or a string. The parser strips whitespace. Strings with `0x` or `0X` use base 16; other strings use base 10. Python `int()` accepts signs and underscores. Invalid strings raise its conversion error. Other types raise `Invalid address value`. |
| CRC32 | lane `crc` | A hexadecimal string of 8 digits. The check ignores case. |
| SHA-1 | lane `sha1` | A hexadecimal string of 40 digits. The check ignores case. |

All code addresses must be even (word-aligned) where the table below says so.

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

[[hooks]]
address = 0x55c2
symbol = "f3_landmakr_video_hook"
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

In the shipped configs, `size` is `2097152` (2 MiB) and each lane file has `524288` bytes.

| Lane offset | `landmakrj` file | `landmakr` file |
| --- | --- | --- |
| 0 | `e61-13.20` | `e61-19.20` |
| 1 | `e61-12.19` | `e61-18.19` |
| 2 | `e61-11.18` | `e61-17.18` |
| 3 | `e61-10.17` | `e61-16.17` |

::: info
The runtime does not read this file. `RomSet::load()` in `runtime/rom.cpp` has its own table of file names and CRC32 values for both sets. It also loads the sprite, tile, sound and sample ROMs, which this config does not describe.
:::

## [discovery]

This section is optional. It controls how `discover()` finds code. Without it, all keys have their default values.

| Key | Type | Required | Default | Validation | Meaning |
| --- | --- | --- | --- | --- | --- |
| `coverage` | string | no | `"recursive"` | Must be `"recursive"` or `"all_aligned"`. Else: `Unknown discovery coverage mode`. | `recursive` follows code from the seeds. `all_aligned` decodes every even address of the ROM independently. |
| `entry_points` | array of addresses | no | `[]` | Each address must be even. Else: `Entry point must be word-aligned (even)`. An address outside the ROM is ignored. | Extra code start addresses. The recompiler treats them as proven seeds. |
| `scan_jump_tables` | boolean | no | `true` | none | Look for jump tables with the pattern scanners. |
| `scan_task_traps` | boolean | no | `true` | none | Look for task entry points that are passed to `TRAP #1`. Only in `recursive` mode. |
| `scan_callbacks` | boolean | no | `true` | none | Look for validated callback addresses that code stores to RAM. Only in `recursive` mode. |
| `inline_string_helpers` | array of addresses | no | `[]` | Each value must be a valid address. | Addresses of subroutines that read a text string stored right after the call. |
| `jump_tables` | array of tables | no | `[]` | See below. | Explicit jump tables. |
| `actor_scripts` | table | no | `{}` | See below. | Description of the actor bytecode. Only in `recursive` mode. |

### Coverage modes

| Mode | What it does | When to use it |
| --- | --- | --- |
| `recursive` | Starts at the 68020 vector table, the `entry_points`, the hook addresses, and the seeds from the scanners. It follows branches and calls. | A game without a full list of code addresses. A scan cannot prove that skipped bytes are data. |
| `all_aligned` | Decodes each even ROM address independently. It records failed decodes in `invalid_pcs`. The emitter registers decoded addresses, including overlapping starts. | Land Maker Japan. This avoids missing valid ROM targets because recursive discovery did not reach them. |

Registration does not prove that every computed jump succeeds. Odd addresses, invalid decodes, RAM targets and unsupported instructions still need runtime handling.

In `all_aligned` mode, the addresses that are already decoded are not decoded again by the recursive walk. So `jump_tables` and `inline_string_helpers` have little or no effect in this mode. `[INFERENCE]` from the control flow in `discover()`; no shipped config combines them.

### Seeds

The recompiler builds the start list in this order: the 68020 vector table, `entry_points`, hook addresses (from `[[hooks]]`), `trap #1` task targets, validated callbacks and actor script callbacks. The first three are *proven* seeds. The others are *speculative* seeds. Both lists appear in `coverage.json`.

### [[discovery.jump_tables]]

Each table gives the targets of one indirect jump or call. Without this, the scanners must find the targets.

| Key | Type | Required | Default | Validation | Meaning |
| --- | --- | --- | --- | --- | --- |
| `address` | address | yes | none | Must be even. A table without `address` is ignored. | Address of the `JMP` or `JSR` instruction that uses the table. |
| `targets` | array of addresses | no | `[]` | Each target must be even. Targets outside the ROM are dropped. | Targets that you list directly. |
| `table` | address | no | none | If set, `count` is required. The table must fit in the ROM. Else: `Jump table outside ROM`. | Address of a table of 32-bit big-endian pointers. |
| `count` | integer | with `table` | none | Must be 0 or more. | Number of pointers in `table`. The loader keeps the pointers that are even and inside the ROM. |

### [discovery.actor_scripts]

This table describes a bytecode that the game uses for actor scripts. The recompiler follows the bytecode to find callback addresses. If the table is empty, the recompiler skips this step. If the table is not empty, the six required keys must all be present. A missing key raises `KeyError`, and the program prints `f3-recomp: 'key'`.

| Key | Type | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `operand_bytes` | array of integers | yes | none | For each opcode value (the index), the number of operand bytes. A negative value marks an invalid opcode. |
| `code_pointer_opcodes` | array of integers | yes | none | Opcodes whose first operand is a code address (a callback). |
| `pointer_field` | integer | yes | none | Displacement of the object field that holds the script pointer. The scanner looks for code that stores a script address in this field (for example `MOVE.L #script,d16(An)`). |
| `return_opcode` | integer | yes | none | Opcode that ends a script. |
| `call_opcode` | integer | yes | none | Opcode that calls another script. |
| `jump_opcode` | integer | yes | none | Opcode that jumps to another script. |
| `pointer_table_strides` | array of integers | no | `[4]` | Strides to try when reading a script pointer table. |
| `entry_points` | array of addresses | no | `[]` | Extra script start addresses. |
| `pointer_tables` | array of tables | no | `[]` | Tables of script pointers. Each table has `table` (address) and `count` (integer). A missing key raises `KeyError`. |

## [[hooks]]

A hook calls a C function of the runtime before a chosen instruction runs. The shipped Japan config has 69 hooks. All of them use the symbol `f3_landmakr_video_hook`. The comment in the config says that the hooks observe game-owned tile block descriptors, and that the native instructions still execute.

| Key | Type | Required | Default | Validation | Meaning |
| --- | --- | --- | --- | --- | --- |
| `address` | address | yes | none | Must be even (`Hook address must be word-aligned`). At emit, the address must be a decoded instruction (`hook address is not discovered code`). Each address can appear once (`duplicate hook`). | Address of the instruction that gets the hook. |
| `symbol` | string | yes | none | Must match `[A-Za-z_][A-Za-z_0-9]*` (`invalid hook C identifier`). | Name of the C function. |

Behavior and limits:

- The emitter declares `extern void SYMBOL(f3_cpu *cpu);` in each generated C file.
- Before the instruction, the generated code calls `f3_cc_flush(cpu)` and then `SYMBOL(cpu)`.
- After the call, the generated block returns at once if `cpu->pc` changed, or if `cpu->stopped` or `cpu->halted` is set. Otherwise the instruction runs.
- A hook address is also a discovery seed.
- The program that links the generated code must define the symbol. The runtime defines `f3_landmakr_video_hook` in `runtime/game_video.cpp`.
- The emitter uses `int(hook["address"])`, without an explicit base. Prefer TOML integers such as `0x55c2`. Quoted decimal strings work. Quoted hexadecimal strings work during discovery but fail during emission.

## What the config does not contain

- Memory map, video and sound ROM files: the runtime has these fixed in `runtime/rom.cpp`.
- Timing tables: they are the CSV files `recomp/68000_cycles.csv` and `recomp/68020_cycles.csv`. See [Generated files](/reference/generated-files).
- The sound CPU: `tools/compile_sound.py` does not use a TOML config. Its options are on the command line.

For how discovery and emit use these keys, read [Discovery](/developer/recompiler/discovery) and [ROM and config](/developer/recompiler/rom-and-config).
