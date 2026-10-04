# Land Maker game-data video

Target: supplied **Japan 2.01J (`landmakrj`)** program, based on `checkpoint-1-coverage` (`46d0dad`). Addresses below are for that program, not the World revision. Generated program C and ROM/asset bytes remain untracked.

## Boundary and evidence

The game renderer observes native display-building routines. Hooks do not replace instructions, advance guest time, change registers or write guest memory. `GameMemory` permits only program-ROM and main-work-RAM reads; it deliberately cannot read FDP memory. Game tile blocks become semantic cells (tile, palette, pen mask, flips and blend selector), rather than a second byte-for-byte graphics-RAM image. A graphics-write observer checks **only producer PC and destination address** to detect missing hooks; it never supplies write values to the scene.

`runtime/video.cpp` remains the independent FDP oracle. Immutable decoded ROM textures are shared to avoid a duplicate 16 MiB asset decode. Oracle layer readback is diagnostic only; it does not advance sprite latches. The incremental proof below starts with complete logical PF0 texture planes and then establishes nine-layer and final composited-frame parity.

Color indices resolve through the shared FDA palette RAM; this is a color asset, not FDP geometry. The game compositor takes semantic maps, decoded glyph pens, semantic sprite geometry and semantic row descriptions. Its normal path never reads FDP tile/sprite/line memory. Runtime mode `fdp` remains the default; `game` composes the supported scene and maintains only the oracle sprite latch for exact fallback, while `compare` renders both and rejects any supported-frame RGB difference. A fallback report names the component, producer PC, count and first/last affected frame; it is not CPU interpreter fallback.

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

## ROM producer map

| Routine | Game-owned input / function |
| --- | --- |
| `$00136e..$001466` | Scroll uploader: main-RAM PF 32-bit positions `$400116..$400132`, pivot words `$400136/$40013a`, calibration table `$4000d8`, control word `$4000ec`, flip flag `$40013e` |
| `$004498..$0044ae` | Sprite task: initializes, yields, compiles game queues at `$4528`, submits at `$4480`, resets queues at `$44b0` |
| `$004688`, `$0046c0` | Game sprite descriptor expansion; these are before the FDP-record output, not hardware-list readers |
| `$0056e6` | Text rectangle descriptor uploader; its loop/count convention differs from PF copies |
| `$0057a2`, `$0057cc`, `$00581c`, `$005856` | String, hexadecimal, bit and decimal text producers |
| `$005b80`, `$005bac`, `$005bce` | ROM/RAM glyph uploads with 16-bit word swap |
| `$005cd8/$005d10` | ROM line-profile initialization and selector setup; exact row ranges are detailed below |
| `$091490..$091612`, `$091834` | Character-select water mixing/clipping |
| `$09d66a..$09d812` | Board palette gradient, perspective zoom/rowscroll and column scroll |
| `$09ecb0..$09ecf4` | Selection PF0 sine-wave rowscroll from ROM table `$1c84` and phase at `A5-$6f6` |
| `$0fe620..$0ff3d0`, table `$0ff102` | Ending slide clipping and transitions |
| `$000c3c/$000c48`, producer `$000c88` | Palette queue: count `$418220`, 8-byte records at `$417040` containing color count, game source pointer and palette byte offset |

The common frame handler `$1110` calls `$136e`, input handling, coin handling, palette dispatch and sound dispatch. Runtime scanout occurs at VBSTART before IRQ2; the oracle then prepares the current sprite list for the following rendered frame. A callback at `$1134` alone is not assumed to represent every completed display task.

### Text and sprite source contracts

The text map is represented by glyph/palette/flip cells and decoded glyph pens, not copied FDP bytes. `$56e6` consumes `u16 rows, u16 columns, u16 glyphs[]`; its stack arguments are source pointer at `SP+4`, destination at `SP+8`, attribute word at `SP+12`. `$5726` instead consumes byte glyphs, reverses columns and XORs the attribute byte with `$40`. `$5768` erases the described rectangle with glyph zero. `$59bc` fills the complete map with `$0290`; `$59ee` fills 30 rows by 40 columns. `$5be0/$5c08` set the first/second column in 29 rows.

`$57a2` consumes a NUL-terminated string with the same source/destination positions and a palette number at `SP+12`. Numeric producers `$57cc/$581c/$5856` take a 32-bit value at `SP+4`, digit count at `SP+8`, destination pointer at `SP+10`, palette at `SP+14`. Hexadecimal and decimal write right-to-left; the bit display writes `H`/`L`. `$5b80/$5bac/$5bce` consume `D0.W+1` 32-bit glyph rows at `A0`, swapping the two words before nibble decoding. The last uploads at glyph `$a0`; the others start at glyph zero and clear glyph `$90`.

Glyph `$90` is also a game cover/fade primitive: `$8de56` blanks it, `$8e9c6/$a1170` fill it, and `$8e0a6/$8e0dc` apply two ROM offset/mask pairs at `A1` to the glyph based at `A0`. `$9b530` fills glyph `$60`; `$9b544` uses it across 42 columns in rows 0–5 and 26–28. Unknown text/glyph writers invalidate the affected semantic state rather than reading hardware RAM back.

Game sprite queues contain 18-byte entries: `+0` graphics descriptor pointer, `+4` zoom long, `+8/+10` X/Y words, `+12` palette word, `+14/+16` horizontal/vertical flip words. Their bases are `$408cf0/$408870/$408630/$408510/$4083f0/$408360/$408120/$407ee0`. `$4528` visits queues 4,5,6,7, then the master-object list, then queues 0,1,2,3.

At `$4688/$46c0`, `A4` points four bytes into the queue entry and `A0` follows the graphics descriptor's 32-bit header; `D7` holds that header. The grid header stores rows-minus-one in its upper word and columns-minus-one in its lower word; tile entries are column-major packed attribute/code longs. `$4480` submits a completed batch. The scene preserves the game's **integer tile-origin quantization**, per-tile fractional raster step and one-frame scanout lag; it does not display the current queue immediately.

| Sprite hook | Game source and scene operation |
| --- | --- |
| `$0041d0` | Reinitializes sprite ownership and clears all three semantic batches. |
| `$0043b0` | Global signed-12-bit scroll from main RAM `$407a16/$407a1a`. |
| `$0043e0` | Command at `$407a1e`: screen flip bit 13, extra pen planes bits 8–9, framebuffer-retention bit 1. |
| `$004528/$004480` | Begin staging / submit completed active batch; scanout latches the submitted batch after the current frame. |
| `$004688` | Single-tile descriptor: attribute/code words at `A0`; queue zoom bytes `A4+1/+3`, coordinate words `+4/+6`, palette/flip words `+8/+10/+12`. |
| `$0046c0` | General column-major grid described above; scaled grids quantize each tile origin and mask the low four X raster-zoom bits. |
| `$0a8f38` | Master-object grid at `A0`; positions in `D1.W/D2.W`, palette/flags in `D3.W`, per-tile attribute XOR. |
| `$0a8f84` | Three-tile object: common attribute XOR `A0+4`, codes `+6/+10/+14`, positions `(x+5,y)`, `(x,y+16)`, `(x+16,y+16)`. |
| `$0a90f4` | Four-tile object: codes `A0+6/+10/+14/+18`, column-major 2×2 placement, palette/flags from `D3.W`. |
| `$0a913c` | Scaled master grid: descriptor at `A0`, object at `A3`, zoom bytes `A3+$10/$12`, flips in low two bits of word `A3+$12`; palette from `D3`. Center offsets are `(columns*zoomX+8)/32` and `(rows*zoomY+8)/16`. |

These contracts come from the ROM instruction stream. Text-plane parity: `--seed {5,6,7} --frames 6000 --video-diff --video-layer-mask 256` compares 46 complete 512×512 text textures per seed, **12,058,624 indexed pixels per seed / 36,175,872 total, zero mismatches**. Native block counts are 80,338,232 / 79,921,207 / 81,856,920; all three runs execute zero fallback instructions. The sprite and complete-scene proofs below are separate subsequent milestones.

The longer text-only seed-5 run also passes **40,000 frames**, 329 sampled text planes / **86,245,376 indexed pixels**, zero mismatches; 534,492,746 native blocks, zero fallback instructions. Frame CRC `$1101a39b`.

### Sprite quantization evidence

The first sprite mismatch was not an FDP-chain feature: seed 5, frame 1080, sprite group 3 had **545 mismatching indexed pixels**, first visible coordinate `(0,104)`. The captured native records for tiles `$6bb3/$6bb4/$6bb5` all have integer Y `$0068`, scale Y `2/256` and attribute `$00e2` (no chain bits). The initial semantic implementation incorrectly retained their intermediate fractional Y positions as three distinct tile origins.

ROM `$480c..$4a36` uses half-pixel carry accumulators but uploads only integer coordinate words at `$4882/$4886` and the corresponding reversed loops. Horizontal tile placement uses all eight zoom bits; the actual tile-width zoom word masks off the low four X bits at `$4812`. `GameSprites` now preserves that distinction. `$a913c..$a93a2` uses the same coordinate quantization and X raster mask, with centering offsets and flip flags from the master object's `$10/$12` fields. A descriptor regression covers a mirrored 3×3 grid with Y scale `2/256` and a non-multiple-of-16 X zoom; it checks the integer tile origins and separate raster width.

After that correction, seed 5 × 6000 frames passes all four sprite priority planes: **46 samples / 3,415,040 indexed pixels per group, each zero mismatches**. The comparison domain is the visible crop of the sprite plane prepared for the *next* frame, matching the oracle's one-frame lag. `f3rt-check` passes the new quantization regression. Final composition remains separately checked.

Continuous sprite-plane sampling subsequently exposed a one-pixel top-edge leak at seed 5, frame 3404; coarse complete-frame sampling exposed the same issue as two RGB pixels at frame 21960. A sprite at scanout Y=11 with Y scale `208/256` ends exactly at Y=24. The oracle rejects its nominal fixed-point rectangle before the `+255` texel-row rounding; omitting that cull lets the last row enter the active image. The game raster now applies the same geometric cull before sampling, including for expanded viewports. A permanent regression failed before this correction and passes after it, while also checking the adjacent partially visible sprite. This is the retained oracle's observed compatibility rule, not an assertion about an unmeasured physical-chip edge case.

### Line-profile source contracts

| Hook / producer | Semantic input and verified operation |
| --- | --- |
| `$010044` | Captures the boot uploader's game register value for PF and pivot controls. |
| `$00136e` | Converts game-owned positions/calibration values from the producer-map addresses into native scanout origins; preserves word arithmetic, NOT/ASR rounding and control semantics. |
| `$005cd8` | Reads ROM profile table `$5d74`; writes exactly 232 rows starting at row 24 normally or row 0 when the game flip byte is set. Blanking rows are not overwritten. |
| `$005d10` | Initializes selectors, not profile values; `$626000` (or `$6261fe` flipped) becomes `$0800`. |
| `$005a22/$005a5e` | Clear PF0/PF1 rowscroll. |
| `$005a9a/$005af4` | Clear PF2/PF3 rowscroll and column scroll and set their X zoom to unity. The low byte of the PF3 zoom register controls **PF1** Y step; the mapping is `{0,3,2,1}`. |
| `$08cfba/$08cfe0` | Cover/dim profile: 232 alpha words starting at the row offset stored in `$400140`. Restore consumes the game's saved alpha words in main RAM `$41ce26`, not an assumed constant. |
| `$091490/$091506` | Special selection label windows: 20 rows of text mix `$380f` and PF3 mix `$3800`; normal starts 224/202, flipped starts 199/181. |
| `$0915d2` | Water region rows 176–251: alpha `$bcba`, PF3 mix `$380e`, sprite priorities `$dd81`, sprite blend/pivot control `$00eb`. |
| `$091834` | Water clipping from `D7`: unsigned word arithmetic computes `(60-D7)*6`; the flipped left edge is `($6980-amount)&511`, not a guessed reflection. |
| `$09217c` | Selection transition: all four sprite priorities become 14 on rows 24–215. |
| `$098dba` | All 256 alpha rows become `$babc`; first observed by the missing-writer guard at frame 1080, store `$098dc8`. |
| `$099b5a` | Attract rows 24–248: sprite blend modes `$df`, priorities `$dd88`, pivot control zero. |
| `$099f86` | Rows 0–247: PF2 mix `$700b`, PF3 `$b00d`, alpha `$b7bf`; rows 1–248: sprite priorities `$cc88`, sprite modes 3, pivot control zero. |
| `$09a252/$09a2f6` | Attract fades: task register `D2.W` supplies 248 alpha words. No FDP readback or guessed animation interpolation. |
| `$09a28a` | Rows 0–247: PF1 mix `$700b`, PF3 `$300d`, alpha `$b7bf`. |
| `$09a6e6/$09acbe/$09ad3e` | Rows 0–247: PF2 mix `$700c`, `$700c`, `$300c` respectively. |
| `$09a8de` | Rows 0–247: sprite priorities `$ee88`. |
| `$09d66a` | PF2 mix `$3005` plus a run-length palette-add gradient from ROM `$9d6a8..$9d6d0`. |
| `$09d72a` | Symmetric board X zoom around row 152 normally /128 flipped; zoom is an eight-bit value, including wrap in blanking rows. Rowscroll uses the exact division/remainder conversion in `$9d784..$9d7a8`. |
| `$09d7b6` | Board animated column scroll reads phase `$40790a&127` **after** the task's `$9d7b0` yield. In normal orientation the 152-word lower half crosses into PF3 rows 0–47; clipping upper bits are cleared by those same writes. |
| `$09ecb0` | PF0 wave: signed low-byte phase indexes ROM sine table `$1c84`; the native word negation, long shift and swap determine the rowscroll fraction. |

The first normalized-line failure at frame 600 was PF1 Y `489` versus oracle `0`: the missing PF3→PF1 Y-step mapping. At frame 1440 PF2 Y `0` versus `482` exposed both the omitted upper-half column-scroll writes and the hook placed before the task yield. These are game-producer corrections, not framebuffer exceptions.

### Fresh oracle compatibility

`landmakr --headless --frames 3480 --video fdp --dump-dir build/captures/video-oracle-attract --dump-start 600 --dump-every 120` executes **49,866,062 native blocks, zero fallback instructions**. Comparing those fresh frames with the retained MAME `mame-taps-fixed` captures gives **25/25 exact RGB frames**, 1,856,000 compared pixels, zero mismatches and zero maximum channel error. This validates the unchanged default oracle path independently of HLE composition.

## First complete seeded scene parity

Seed 5 × 6000 frames, every 120 frames from frame 600: **46 samples**, all nine source layers and normalized visible row descriptions match, followed by **3,415,040 final RGB pixels / zero mismatches**. PF0–PF3 each compare 24,117,248 indexed texels; sprite groups 0–3 each compare 3,415,040 visible-plane pixels; text compares 12,058,624 indexed texels. Native execution remains 80,338,232 blocks / zero fallback instructions. This is the base-parity gate before exposing presentation enhancements.

The 6000-frame run reconstructs 5769 frames and delegates 231 startup frames to the oracle: line initialization/POST (229), glyph initialization (1), sprite POST (1), last fallback at frame 418. The normalized line comparison catches geometry, clipping, priority, blend selector/weights and mosaic state independently of the RGB composition.

Continuous no-input attract comparison through frame 3480 now checks **3249 supported frames / 241,205,760 RGB pixels / zero mismatches**, with only the same 231 startup fallbacks. Native execution is 49,866,062 blocks / zero fallback instructions; final CRC `$b490d7d9`. Earlier runs exposed the ordinary attract profiles listed above via the missing-writer guard; each was reconstructed from its ROM producer before accepting its frames.

Extended runs with all nine layer bits selected pass seeds **5, 6 and 7 × 40,000 frames**. Each seed compares **329 samples**: 172,490,752 indexed texels per PF, 24,424,960 pixels per sprite group, 86,245,376 text texels and **24,424,960 final RGB pixels**, all zero mismatches. Native blocks are 534,492,746 /529,768,165 /536,649,307, respectively, with zero CPU fallback. Final CRCs are `$1101a39b` /`$54a2ed76` /`$e9a0299a`. Each run has only 231 startup renderer fallback frames, last at frame 418. Seeds 5 and 6 include the corrected nominal sprite-edge cull.

The final continuous seed-5 run selects **every frame 600–4000**: 3401 samples, 1,783,103,488 pixels per PF, 252,490,240 per sprite group, 891,551,744 text pixels and **252,490,240 final RGB pixels**, all exact. It executes 54,087,373 native blocks with zero fallback, including the formerly failing sprite-edge frame.

Fresh `--video game` attract captures independently match **25/25 retained MAME frames**, 1,856,000 RGB pixels, zero mismatches/max channel error. All 25 samples are beyond the last startup fallback. The run uses 49,866,062 native blocks, zero CPU fallback and CRC `$b490d7d9`, matching the separately refreshed FDP oracle mode. Artifacts: `build/captures/video-game-attract`.

## Evidence conflict

`graphics-structs.txt` labels packed playfield bit 31 as horizontal and bit 30 as vertical. The existing oracle does the opposite. The game's own `$565e` dispatch table sends bit-30-only input to the negative-column-step helper `$5678`, and bit-31-only input to the negative-row-step helper `$5686`. The game renderer therefore uses bit 30 for horizontal and bit 31 for vertical; this remains explicit in `GameTiles::put`. This is ROM-backed behavior, not a claim that the work-in-progress hardware notes are authoritative.

Clipping has a separate unresolved hardware-evidence conflict inherited from the baseline investigation: pinned MAME `cfc4760a` and y-ack `fdp-collapse` `28e411d4f760df3d55fae070a2f6424f89966a2f` use an inverted-plane `max(range.left, endpoint)` combiner, while WIP `clip.txt:157–190` proposes a bitmask/union model with different no-plane/global-invert cases. The independent game compositor retains the baseline rule and now matches the exercised game water/selection clipping profiles. Neither those profiles nor the unavailable die material establish every inverted multi-plane combination on physical hardware; no speculative alternate implementation is substituted.

## Actually exercised features and limits

The measured normal-orientation runs exercise all four 64×32 playfields, tile flips/palette XOR/blend selectors, ROM sprite grids and fixed/scaled master objects, programmable text/glyph fades, per-line priorities and alpha profiles, selection water clipping, PF0 sine rowscroll, and the board's perspective X zoom, Y step, palette-add gradient and column scroll. Sprite extra-pen-plane use is concrete: the frame-3404 capture has game command `$0101` at `$407a1e`, selecting a 5-bit pen mask. Hardware-sized map wrapping and tile-origin quantization remain part of game geometry.

Rows are normalized to a game scene before composition: four PF layers, four sprite priority groups, text, four clipping intervals, four blend weights, palette background, per-PF X/Y phase and step, and text position. Equal-priority order is text, SP0, PF0, SP3, PF3, SP2, PF2, SP1, PF1. The compositor uses opaque, normal and reverse eight-step alpha modes and explicit source/destination priority state. Mosaic state is decoded and compared, but no nontrivial mosaic animation is claimed as exercised by these runs.

| Unsupported HLE case | Exact-visible behavior and recovery |
| --- | --- |
| POST / incomplete initial ownership | Oracle only for the 231 measured startup frames through frame 418: line producer `$1003a` (229), incomplete glyphs (1), sprite POST writer `$10412` (1). Known profile/map/glyph/sprite initialization establishes ownership. |
| Ending transitions `$fe620/$fefe6/$ff0fa` | Explicit line-component invalidation; oracle rendering until the known line-profile reinitialization. These endings are not claimed as reconstructed or exercised. |
| Bitmap pivot layer | Explicit `bitmap-pivot` frame fallback. The programmable text layer is implemented; bitmap-pivot geometry is not. |
| Global screen flip / retained sprite framebuffer | Explicit `flipped-screen` / `sprite-trails` frame fallback while the command remains active. Their command bits and descriptors are decoded, but complete flipped/trail scanout is outside the measured contract. |
| Unknown FDP producer or unsupported descriptor source/range | Guard uses only PC/address; invalidates the affected component. No write-value readback, fabricated scene or silently skipped layer. Complete known PF/text/profile initialization or sprite reset restores ownership as appropriate. |
| Oversized sprite grids/batches | Descriptor rejection rather than truncation: supported grids are at most 32×32 tiles and batches at most 1024 sprites. |

The default oracle path remains available for every frame. The game path retains the oracle's sprite lag each frame, even while not using its composition, so fallback has the correct preceding sprite plane. A two-machine strict-native smoke deliberately injected an unknown PF0 writer at frame 2392: both native images matched through all 2400 frames, and exactly the last eight frames used the reported `$222220` fallback. This proves transition into fallback after sustained game rendering, not merely cold-start behavior.

## Opt-in presentation

These options were introduced only after the complete seeded base-parity gate above:

| Option | Default | Operation |
| --- | --- | --- |
| `--video-scale 1..4\|auto\|auto-integer` | `1` | Rerasterizes scene geometry at the requested internal resolution. GPU auto modes follow physical window pixels (ceiling/aspect fit or floor/exact nearest integer fit), clamped to 1–4. PF fractional X/Y sampling and sprite zoom are evaluated at the higher resolution; this is not enlargement of the finished native RGB frame. Original ROM textures/glyphs remain the artwork. |
| `--video-border 0..160` | `0` | Adds that many native scene columns on each side. `48` gives a 416×232 viewport, approximately 16:9. Native game logic and HUD layout are not widened; off-screen map content can be empty or wrapped. |
| `--video-filter nearest\|linear` | `nearest` | Optional SDL presentation-texture filtering. Linear filters the final display texture; it does not claim higher-detail source art or alter native captures. |

Options require `--video game` or `compare`. The invariant native `Machine::pixels` stays 320×232 for comparison, captures and CRCs. Presentation buffers are allocated once only when scale/border are enabled. Unsupported frames preserve the exact oracle picture, integer-scaled in the center, with black added columns; no invented ending/bitmap/flip geometry is extrapolated.

GPU runtime scaling changes only host geometry/resources, retaining
constructor-fixed canonical presentation buffers and trail history.
Headless auto stays at scale 1; netplay requires fixed scale 1/border 0.
Automatic policies, measured 5–8x cap decision and resize/fullscreen evidence:
[GPU-VIDEO.md](GPU-VIDEO.md#automatic-internal-resolution-phase-6).

Two independent real machines, one FDP and one `game`, ran the seed-5 input schedule for 2400 frames at scale 1 /border 48 and scale 2 /border 48, including the injected fallback above. **178,176,000 native RGB pixels per run matched**, and PC, cycles and D/A registers remained equal; CPU fallback was zero. Scale 1's native center matched in every frame. Nonblack off-screen content reached all 22,272 added pixels at frame 1407 (89,088 at 2×); 2× rerasterization differed from nearest-enlarged native RGB in 5,450,015 pixels across the run, peaking at 11,850 at frame 1409.

Actual Cocoa/Metal frontend runs through attract frame 1920 produced **832×464 internal images** in **1248×696 SDL surfaces**, with scale 2 /border 48, for both filters. The captured surfaces were visually inspected. Nearest versus linear changes 172,330 RGB surface pixels while both runs retain native CRC `$3fadf226`, 28,866,756 native blocks and zero CPU fallback. Captures are under `build/captures/game-wide-2x-{nearest,linear}.{bmp,png}`; generated evidence is not committed.

Maximum-border smoke also passes scales **3 and 4 /border 160** through 1500 seeded frames: actual internal dimensions **1920×696 /2560×928**, 111,360,000 exact native RGB pixels per run, equal CPU state and zero fallback. Compared with nearest-enlarged native RGB, the central scene has 6,937,948 /13,969,522 rerasterized pixels across the runs. The final Cocoa/Metal `game` frontend also passes through frame 3480 at scale 2 /border 48 /linear, with native CRC `$b490d7d9`; its saved surface `build/captures/game-final-metal.png` was visually inspected.

CLI smoke rejects scale 0/5, border 161, an unknown filter and attempted enhancements in `fdp` mode with explicit errors. `f3rt-check` passes both geometry regressions and the descriptor-source regression; no ROMs, generated C, captures or throwaway verification programs are committed.

## Acceptance

The scoped phase is complete: ROM-owned scene reconstruction, independent oracle selection, exact incremental and extended seeded parity, preserved strict-native/MAME acceptance, explicit fallback limits and opt-in presentation are implemented and exercised. The unsupported cases listed above remain deliberate oracle fallbacks, not claims of complete physical FDP coverage or verified ending/flip/trail HLE.
