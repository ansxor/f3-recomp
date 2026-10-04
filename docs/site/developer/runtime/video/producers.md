# Producer hooks and write guards

**What you will learn:** how a hook gets from `games/landmakrj/config.toml` into the running game, what each of the 69 hooks watches, and how the write guards find a producer that has no hook.

All addresses are for the Japan program `landmakrj`. They are not valid for the World revision `landmakr`.

## From config entry to function call

A hook is a `[[hooks]]` table in `games/landmakrj/config.toml`:

```toml
[[hooks]]
address = 0x55c2
symbol = "f3_landmakr_video_hook"
```

The recompiler generator (`recomp/generate.py`) reads the entries and checks them:

- the `symbol` must be a valid C identifier;
- the `address` must be an instruction that discovery found (otherwise the generator stops with an error);
- two hooks must not share an address.

For each hooked address the generator emits this code at the start of the instruction (see [Emission](/developer/recompiler/emission)):

```c
L_0055c2: {
    f3_cc_flush(cpu);
    f3_landmakr_video_hook(cpu);
    if (cpu->pc != 0x000055c2u || cpu->stopped || cpu->halted) return;
    /* ... code of the instruction itself ... */
```

`f3_cc_flush` writes the lazy condition codes into the status register, so the hook sees a correct `cpu->sr`. The hook runs **before** the instruction. Registers and the stack hold the values the routine sees at its first instruction.

At run time `f3_landmakr_video_hook` calls `GameVideo::observe()` (see [GameVideo](/developer/runtime/video/game-hle)). The function reads `cpu.pc` and each component runs a `switch` on it. A component ignores a PC that it does not know. Several components can use the same PC: the PF clear hooks 0x5a22, 0x5a5e, 0x5a9a and 0x5af4 serve both `GameTiles` and `GameLines`.

::: warning
You must regenerate the C code after you change a `[[hooks]]` entry. The hooks are part of the generated program. A new `case` in `observe` does nothing until the matching hook exists in the generated code. See [Build pipeline](/developer/build-pipeline).
:::

## Why the PC is the hook key

The generated code sets `cpu->pc` to the next instruction address **after** the instruction body. During the body, `cpu->pc` still holds the address of the instruction. When the instruction stores to video RAM, `Machine::write8` sees `cpu.pc` equal to the address of the storing instruction. The write guards use this value as the **producer PC**.

## Calling convention seen by the hooks

Some producer helpers use a stack calling convention. Entry hooks see their arguments before `LINK A6`. Other routines and internal hooks read register arguments.

| Offset from `A7` | Content |
| --- | --- |
| `+0` | Return address |
| `+4` | First argument |
| `+8` | Second argument (a long) |
| `+12` | Third argument |

The per-layer pages show the exact arguments of each hook. Hooks inside a routine (for example 0x4688 and 0x9d7b6) read registers such as `A0`, `A3`, `A4`, `A5`, `D1`, `D2`, `D3`, `D7`, as the instruction stream of the game uses them.

## The 69 hooks

The table lists every hook of `landmakrj`. The column "Component" names the class that handles it. "Lines" means `GameLines`.

### Playfield tiles (9 hooks)

| Address | Game operation | Component |
| --- | --- | --- |
| 0x55c2 | Copy a tile-block descriptor with a palette word and an attribute XOR | Tiles |
| 0x5614 | Copy a tile-block descriptor with an attribute XOR | Tiles |
| 0x56ae | Erase a rectangle | Tiles |
| 0x5a22 | Clear PF0 | Tiles and lines |
| 0x5a5e | Clear PF1 | Tiles and lines |
| 0x5a9a | Clear PF2 | Tiles and lines |
| 0x5af4 | Clear PF3 | Tiles and lines |
| 0x9bcea | Reset all four maps for a match | Tiles |
| 0x9ec4e | Fill the side strips of the selection screen | Tiles |

### Text (21 hooks)

| Address | Game operation |
| --- | --- |
| 0x56e6 | Glyph rectangle with 16-bit glyph numbers |
| 0x5726 | Glyph rectangle with byte glyph numbers, columns reversed |
| 0x5768 | Erase a text rectangle |
| 0x57a2 | Draw a NUL-terminated string |
| 0x57cc | Draw a hexadecimal number |
| 0x581c | Draw a bit pattern (`H` and `L`) |
| 0x5856 | Draw a decimal number |
| 0x59bc | Clear the whole text map to glyph 0x90 with palette 1 |
| 0x59ee | Fill 30 rows by 40 columns with the value 0x0290 |
| 0x5b80, 0x5bac | Upload glyph pixel rows from `A0`, starting at glyph 0 |
| 0x5bce | Upload glyph pixel rows from `A0`, starting at glyph 0xa0 |
| 0x5be0, 0x5c08 | Fill the first or second column of 29 rows |
| 0x8de56 | Blank glyph 0x90 |
| 0x8e9c6, 0xa1170 | Fill glyph 0x90 with pen 15 |
| 0x8e0a6, 0x8e0dc | Apply two ROM mask pairs to a glyph (set or clear bits) |
| 0x9b530 | Fill glyph 0x60 with pen 15 |
| 0x9b544 | Cover rows 0 to 5 and 26 to 28 with glyph 0x60 (value 0x0260) |

### Sprites (11 hooks)

| Address | Game operation |
| --- | --- |
| 0x41d0 | Sprite initialization. Resets sprite ownership. |
| 0x43b0 | Global sprite scroll from work RAM 0x407a16 and 0x407a1a |
| 0x43e0 | Sprite command word at work RAM 0x407a1e (flip, pen planes, trails) |
| 0x4528 | Start of a new staging batch |
| 0x4480 | Submit the completed batch |
| 0x4688 | Compile one tile from a queue entry |
| 0x46c0 | Compile a grid of tiles from a queue entry |
| 0xa8f38 | Master object: grid |
| 0xa8f84 | Master object: three tiles |
| 0xa90f4 | Master object: four tiles |
| 0xa913c | Master object: scaled grid |

### Line effects (28 hooks)

| Address | Game operation |
| --- | --- |
| 0x10044 | Boot: capture the register value for the control words |
| 0x136e | Display register uploader (scroll values) |
| 0x5cd8 | Initialize the line profile from the ROM table at 0x5d74 |
| 0x5d10 | Set up the line latches |
| 0x8cfba, 0x8cfe0 | Cover and dim profile: alpha for 232 lines |
| 0x91490, 0x91506 | Label windows of the character-select screen |
| 0x915d2 | Water region (rows 176 to 251) |
| 0x91834 | Water clip animation |
| 0x9217c | Selection transition: sprite priorities |
| 0x98dba | Alpha profile for all 256 lines |
| 0x99b5a | Attract mode: sprite modes and priorities |
| 0x99f86 | Attract mode: PF2 and PF3 mix, alpha, sprite priorities |
| 0x9a252, 0x9a2f6 | Attract fades |
| 0x9a28a | Attract: PF1 and PF3 mix |
| 0x9a6e6, 0x9acbe, 0x9ad3e | Attract: PF2 mix |
| 0x9a8de | Attract: sprite priorities |
| 0x9d66a | Game board: PF2 mix and palette-add gradient |
| 0x9d72a | Game board: X zoom and row scroll |
| 0x9d7b6 | Game board: column scroll |
| 0x9ecb0 | Selection screen: sine-wave row scroll on PF0 |
| 0xfe620, 0xfefe6, 0xff0fa | Ending slides. Mark the lines component unsupported. |

Count: 9 + 21 + 11 + 28 = 69, which matches the number of `[[hooks]]` entries in `games/landmakrj/config.toml`.

## Write guards

A hook only works if it covers **every** routine that changes the picture. A game can have a routine you did not find. The write guards detect this. `Machine::write8` calls `GameVideo::observe_write(pc, address)` for:

- every byte write to 0x600000 to 0x63ffff (graphics RAM);
- every byte write to 0x660000 to 0x66001f (control registers).

`GameVideo::observe_write` forwards the call to all four components. Each component looks only at its own address range and has a **list of covered PCs**: the PCs of the store instructions that a hook already models. If the writing PC is on the list, the guard does nothing. If it is not, the guard marks the component unsupported and stores the PC.

| Component | Address range watched | Covered producer PCs |
| --- | --- | --- |
| Tiles | 0x610000 to 0x617fff | 0x55fc, 0x5646, 0x56d6, 0x5a2e, 0x5a6a, 0x5aa6, 0x5b00, 0x9bd08, 0x9bd0a, 0x9bd0c, 0x9bd0e, 0x9ec66, 0x9ec6a, 0x9ec6e, 0x9ec72 |
| Text | 0x61c000 to 0x61ffff | 0x570e, 0x5712, 0x5756, 0x5758, 0x578e, 0x57bc, 0x57be, 0x580c, 0x580e, 0x583a, 0x5840, 0x5846, 0x58c2, 0x59d0, 0x5a08, 0x5bf8, 0x5c20, 0x5b8a, 0x5b9a, 0x5bb6, 0x5bc6, 0x5bd8, 0x8de60, 0x8e0b2, 0x8e0c2, 0x8e0e6, 0x8e0f4, 0x8e9d2, 0x9b53a, 0x9b562, 0x9b564, 0x9b586, 0xa1176, 0xa117c, 0xa1184, 0xa118c, 0xa1194, 0xa119c, 0xa11a4, 0xa11ac |
| Sprites | 0x600000 to 0x60ffff | Ranges 0x41d0 to 0x4380, 0x43b0 to 0x43de, 0x43e0 to 0x43fe, 0x4422 to 0x447e, 0x4688 to 0x46be, 0x46c0 to 0x480a, 0x480c to 0x4a36, 0xa8f38 to 0xa8f82, 0xa8f84 to 0xa8fce, 0xa9036 to 0xa9076, 0xa90f4 to 0xa913a, 0xa913c to 0xa93a2 |
| Lines | 0x620000 to 0x62ffff and 0x660000 to 0x66003f | 29 ranges in `is_covered_write` in `game_lines.cpp`, for example 0x00136e to 0x00145e (register uploader), 0x005d30 to 0x005d6c (profile init), 0x09d684 to 0x09d6a0 (board gradient) |

Notes:

- Only writes to the **tile map** area matter for tiles. The tile guard compares `address - 0x610000` to find the layer (`/ 0x2000`).
- The text guard watches both map and glyph areas. An unknown map writer clears `map_valid_`. An unknown glyph writer clears the entire glyph completeness byte.
- Pivot RAM has no semantic producer guard. The measured runs do not use bitmap pivot. `GameVideo` rejects any visible row that selects bitmap mode.
- Each component records a producer PC for reports. Replacement rules differ. Tiles and lines can replace it after ownership recovery; text and sprites retain the first nonzero PC until their reset point.

## Timing rules for hooks

- **Observe at the right moment.** The hook at 0x9d7b6 is placed **after** the game task wakes from its frame yield. A hook placed before the yield showed the old phase and caused a mismatch at frame 1440.
- **Hooks fire on every call.** A routine can run many times in a frame. Each hook call must update the scene as the routine would update the video RAM.
- **Reads must stay in ROM and work RAM.** If a hook needs a value that lives only in video RAM, the design is wrong. Find the game variable that the routine reads instead.

See [Extending the renderer](/developer/runtime/video/extending) for the full procedure to add a hook.

Sources: [config.toml](https://github.com/ansxor/f3-recomp/blob/main/games/landmakrj/config.toml), [generate.py](https://github.com/ansxor/f3-recomp/blob/main/recomp/generate.py), and [machine.cpp](https://github.com/ansxor/f3-recomp/blob/main/runtime/machine.cpp). Per-component guard lists live in `runtime/game_*.cpp`.
