# Land Maker game-data video

Target: supplied **Japan 2.01J (`landmakrj`)** program, based on `checkpoint-1-coverage` (`46d0dad`). Addresses below are for that program, not the World revision. Generated program C and ROM/asset bytes remain untracked.

## Boundary and evidence

The game renderer observes native display-building routines. Hooks do not replace instructions, advance guest time, change registers or write guest memory. `GameMemory` permits only program-ROM and main-work-RAM reads; it deliberately cannot read FDP memory. Game tile blocks become semantic cells (tile, palette, pen mask, flips and blend selector), rather than a second byte-for-byte graphics-RAM image. A graphics-write observer checks **only producer PC and destination address** to detect missing hooks; it never supplies write values to the scene.

`runtime/video.cpp` remains the independent FDP oracle. Immutable decoded ROM textures are shared to avoid a duplicate 16 MiB asset decode. Oracle layer readback is diagnostic only; it does not advance sprite latches. The first milestone compares complete logical PF0 texture planes; it is **not yet a final composited-frame parity claim**.

Evidence priority follows `CONTEXT.md`: actual game ROM behavior and observed Land Maker output, primary hardware sources, work-in-progress notes, then MAME source.

References:

- Supplied Japan program, reset instruction `$010004`: `A5 = $408000`.
- [Land Maker investigation](https://qcs.shsbs.xyz/) (`qcscrawl content 4545`), particularly ending clipping and graphics-block layouts.
- Local `~/Workspace/taito-f3/graphics-structs.txt`, `sprite-ram.txt`, `line-ram.txt`, `scroll-regs.txt`.
- [Furrtek TC0630FDP die notes](https://siliconprawn.org/archive/doku.php?id=furrtek:taito:tc0630fdp). Access limitation: both archive hostnames returned HTTP 403; Chromium remained at the Cloudflare challenge. No unread die-page claim is used as evidence.

## Verified tile-block ABI

All multi-byte game values are big-endian. At helper **entry, before `LINK A6`**, stack offsets are:

| Offset | `$55c2` palette/XOR blit | `$5614` XOR blit | `$56ae` rectangle erase |
| --- | --- | --- | --- |
| `SP+4` | Source descriptor pointer | Source descriptor pointer | Source descriptor pointer |
| `SP+8` | Destination cell address | Destination cell address | Destination cell address |
| `SP+12` | Palette XOR word, low nine bits used | Attribute XOR in high word of a long | Unused |
| `SP+14` | Attribute XOR in high word of a long | — | — |

Descriptor: `u16 rows`, `u16 columns`, followed by row-major `u32` tile entries for a copy. Each entry contains attributes in its high word and a tile number in its low word. `$55f0` inserts the palette argument into the attribute-XOR mask, rather than replacing the source cell's palette. `$55fa`/`$5644` XOR that mask with each source entry.

Destination is used solely as a logical layer/cell identifier: `$610000 + layer*$2000 + row*$100 + column*4`, four 64×32 maps. It is never dereferenced by the game renderer. Each map samples 16×16 ROM tiles and wraps to a 1024×512 texture.

Attribute decoding used by the existing observed oracle and the game renderer:

| Attribute bits | Meaning |
| --- | --- |
| `0..8` | Palette row; base index = row × 16 |
| `9` | Blend selector |
| `10..11` | Extra-plane selection; pen mask = `((planes & ~palette_row) << 4) | 15` |
| `14` | Horizontal flip |
| `15` | Vertical flip |

Copy helpers use do/while `SUBQ.W` / signed `BGT`; the erase helper tests the decremented word before writing. Their zero-count behavior differs. Flip helpers `$566e/$5678/$5686/$5698` select ±4-byte column and ±256-byte row steps. At `$5680/$56a0`, bytes `43f1 14fc` encode `LEA -4(A1,D1.W*4),A1`: extension bits 10..9 are `2`, selecting ×4. Capstone's printed operand omits that scale. The first PF1 comparison rejected an unaligned scene destination at frame 1560 and exposed this disassembly-display trap; raw extension bytes, not the misleading operand string, determine the implementation.

### Implemented producer hooks

| ROM PC | Game operation | Scene effect |
| --- | --- | --- |
| `$0055c2` | Descriptor copy with palette and attribute XOR | Expands game ROM/RAM descriptor into semantic cells |
| `$005614` | Descriptor copy with attribute XOR | Same expansion without separate palette argument |
| `$0056ae` | Descriptor-sized erase | Clears the requested logical rectangle |
| `$005a22` | PF0 clear | Clears all 2048 cells; re-establishes complete PF0 ownership |
| `$005a5e` | PF1 clear | Clears all 2048 cells |
| `$005a9a` | PF2 clear | Clears all 2048 cells |
| `$005af4` | PF3 clear | Clears all 2048 cells |
| `$09bcea` | Match playfield reset | Clears all four maps |
| `$09ec4e` | Selection-screen side-strip fill | Uses game `D0.W` tile for two 4×15 rectangles, columns 20–23 and 60–63; attribute `$0c80` |

The corresponding native stores still execute. Guarded store PCs are `$55fc`, `$5646`, `$56d6`, `$5a2e`, `$5a6a`, `$5aa6`, `$5b00`, `$9bd08/$9bd0a/$9bd0c/$9bd0e`, and `$9ec66/$9ec6a/$9ec6e/$9ec72`. Any other playfield producer invalidates the affected HLE layer until a complete known clear. The comparison fails explicitly rather than ignoring the difference. Unaligned destinations and non-ROM/non-work-RAM descriptor sources are also unsupported, not silently approximated.

### First measured milestone

Command in `wt/video`:

```sh
./build/f3rt-gameplay-regression --seed 5 --frames 3600 --video-diff --video-layer-mask 1
```

Results: seeds **5, 6 and 7**, each 3600 frames. Each seed compares 26 PF0 samples at frames 600..3600, step 120: **13,631,488 indexed pixels per seed, zero mismatches**; aggregate **40,894,464 pixels**. Native blocks respectively 48,820,718 / 48,636,786 / 49,747,154; **zero fallback instructions in every run**. The committed harness's real coin/start/LCG gameplay schedule is unchanged.

Comparison includes transparent coverage, palette index and blend selector for visible texels across the entire 1024×512 plane, including off-screen cells. Two transparent texels are the same rendered pixel regardless of their unused palette index. This is stronger than RGB equality for nontransparent texels, but does not yet prove scrolling, clipping, inter-layer mixing or final 320×232 output.

The first run correctly rejected an uncovered producer at seed 5, frame 1320, PC `$09ec66`. ROM inspection identified the side-strip operation above. Adding its high-level routine hook—not copying its FDP writes—made the same scenario pass.

The next incremental run selects all four maps:
`--seed 5 --frames 6000 --video-diff --video-layer-mask 15`.
PF0, PF1, PF2 and PF3 each compare **46 samples / 24,117,248 indexed pixels / zero mismatches**.
The run executes 80,338,232 native blocks with zero fallback. The indexed-reversal correction also passes
`f3rt-check`, including a three-column mirrored descriptor with palette XOR and blend selection,
an unpainted right boundary, unknown-producer invalidation and recovery after a complete game clear.

## Remaining producer map: ROM evidence, not parity claims

| Routine | Game-owned input / function |
| --- | --- |
| `$00136e..$001466` | Scroll uploader: main-RAM PF 32-bit positions `$400116..$400132`, pivot words `$400136/$40013a`, calibration table `$4000d8`, control word `$4000ec`, flip flag `$40013e` |
| `$004498..$0044ae` | Sprite task: initializes, yields, compiles game queues at `$4528`, submits at `$4480`, resets queues at `$44b0` |
| `$004688`, `$0046c0` | Game sprite descriptor expansion; these are before the FDP-record output, not hardware-list readers |
| `$0056e6` | Text rectangle descriptor uploader; its loop/count convention differs from PF copies |
| `$0057a2`, `$0057cc`, `$00581c`, `$005856` | String, hexadecimal, bit and decimal text producers |
| `$005b80`, `$005bac`, `$005bce` | ROM/RAM glyph uploads with 16-bit word swap |
| `$005d10` and surrounding helpers | Line-profile initialization; actual latch/feature use still requires measured coverage |
| `$091490..$091612`, `$091834` | Character-select water mixing/clipping |
| `$09d66a..$09d812` | Board palette gradient, perspective zoom/rowscroll and column scroll |
| `$09ecb0..$09ecf4` | Selection PF0 sine-wave rowscroll from ROM table `$1c84` and phase at `A5-$6f6` |
| `$0fe620..$0ff3d0`, table `$0ff102` | Ending slide clipping and transitions |
| `$000c3c/$000c48`, producer `$000c88` | Palette queue: count `$418220`, 8-byte records at `$417040` containing color count, game source pointer and palette byte offset |

The common frame handler `$1110` calls `$136e`, input handling, coin handling, palette dispatch and sound dispatch. Runtime scanout occurs at VBSTART before IRQ2; the oracle then prepares the current sprite list for the following rendered frame. A callback at `$1134` alone is not assumed to represent every completed display task.

## Evidence conflict

`graphics-structs.txt` labels packed playfield bit 31 as horizontal and bit 30 as vertical. The existing oracle does the opposite. The game's own `$565e` dispatch table sends bit-30-only input to the negative-column-step helper `$5678`, and bit-31-only input to the negative-row-step helper `$5686`. The game renderer therefore uses bit 30 for horizontal and bit 31 for vertical; this remains explicit in `GameTiles::put`. This is ROM-backed behavior, not a claim that the work-in-progress hardware notes are authoritative.

## Acceptance still open

The remaining layers, final compositor, explicit supported-feature/fallback accounting, full seeded per-layer output comparisons, fresh oracle/MAME 25-frame regression and actual frontend mode selection still require verification. Resolution scale, extra border and filtering remain disabled/not exposed until base pixel parity. No phase-2 completion claim is made by this first-component milestone.
