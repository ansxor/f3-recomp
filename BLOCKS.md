A **block** is how the F3 sprite chip draws a multi-tile sprite from a single position. You write the position once, in the first entry, and the chip places the remaining tiles next to it automatically.

## The problem it solves

Each sprite entry is one 16×16 tile. A 48×48 enemy needs 9 tiles. Without blocks, the game has to write X/Y into all 9 entries every frame, which is what Land Maker does. With blocks, it writes X/Y once.

## How the chip does it

Each entry has a few control bits in word 4 (`docs/notes/hardware/sprites.md:142-173`):

| Bits  | Name            | Meaning                                                                                                            |
| ----- | --------------- | ------------------------------------------------------------------------------------------------------------------ |
| 11    | **multi**       | 0 = "the **next** entry starts a new block". 1 = "the next entry continues this block"                             |
| 15:14 | X block control | where this tile goes horizontally: at the block origin, same column as the previous tile, or one tile to the right |
| 13:12 | Y block control | where it goes vertically: at the origin row, or one tile below the previous one                                    |

The chip walks the list in order:

1. **Start a block:** if the previous entry had multi = 0, the current entry's position becomes the **block origin**, and its zoom becomes the block's scale.
2. **Place the following tiles:** each one uses its control bits: "one below the previous", or "start a new column to the right".
3. **End the block:** an entry with multi = 0 ends it, and the next entry starts a new one.

## A 3×3 block

```text
+-----+-----+-----+
|  1* |  4  |  7  |
|     |     |     |
+-----+-----+-----+
|  2  |  5  |  8  |
|     |     |     |
+-----+-----+-----+
|  3  |  6  |  9  |
|     |     |     |
+-----+-----+-----+

1* origin: has X/Y + zoom
2,3: "one below previous"
4,7: "new column to the right"
5,6,8,9: "one below previous"
Only entry 1's position is used.
Columns first, then rows.
```

Moving the whole enemy means changing one X/Y, in entry 1. Zoom works the same way: the chip scales the tile spacing by the zoom from the block's first entry.

## Why this matters for sprite groups

A block is a **sprite group defined by the hardware itself**:

- **Rigid by construction:** every tile's position = origin + fixed tile steps × zoom. That's exactly $\text{pos}(S) = O + \ell(S)$.
- **O is explicit:** the origin entry's X/Y. No analysis needed.
- **Same for every F3 game:** these are chip rules, so detection needs no per-game config.

## The two games

| Game       | How it draws | Result |
|------------|--------------|--------|
| commandw   | Uses blocks: about 10 per frame, holding all ~122 sprites | Groups come free from the sprite list |
| landmakrj  | Never sets multi: every tile carries its own X/Y | Each block is a single tile, so the hardware groups nothing and code tracing has to |

Next: decide whether to start the landmakrj trace test from my last message (4 handlers, does the X/Y trace reach `$c`/`$e`). Say go to start it.
