# Binary-size exclusion experiment

## Measurement checkpoint (before implementation)

Land Maker Japan, Release Clang/arm64, `all_aligned` main and native sound.
Timed fresh configure: **14.864 s**; build (`landmakr` and gameplay harness,
`-j 6`): **54.537 s**. Worktree binary: **84,436,984 bytes**;
Mach-O `__TEXT`: **56,901,632 bytes**. The supplied reference binary is
84,291,656 bytes / 56,770,560-byte `__TEXT`; it predates this worktree's GPU
frontend integration, so this experiment uses its own fresh baseline.

| Provisional ROM region | Native functions | Registered PCs | Function C bytes | Dispatch C bytes | Object `__text` bytes | Dispatch object bytes |
|---|---:|---:|---:|---:|---:|---:|
| Main code and other data | 10,878 | 336,775 | 126,308,600 | 13,274,066 | 25,937,684 | 5,388,400 |
| Main graphics-looking hypothesis, `0x020000..0x088000` | 6,656 | 209,535 | 92,068,080 | 8,206,148 | 18,381,452 | 3,352,560 |
| Main FF padding, `0x007030..0x010000`, `0x11b362..0x1ffffe` | 0 | 486,966 | 0 | 20,452,572 | 0 | 7,791,456 |
| Sound program and other data | 93,326 | 114,501 | 40,359,960 | 4,958,487 | 9,616,948 | 1,832,016 |
| Sound FF padding, `0xc1e45a..0xc20000`, `0xc39a30..0xc80000` | 0 | 147,643 | 0 | 6,053,363 | 0 | 2,362,288 |

C columns attribute complete native functions and dispatch rows by their guest
start address; declarations and file preambles are separate overhead. Object
columns measure symbol extents in Mach-O object `__text` and 16-byte dispatch
records, not relocation/debug/container bytes. All generated C including
preambles/declarations totals **316,308,113 bytes**. Full generated object file
sizes are preserved in the local measurement JSON.

**Upper bound before implementation:** these padding ranges contain no native
functions already; they account for **10,153,744 dispatch bytes**, not 10 MB of
native instructions. Removing padding cannot materially reduce native `__TEXT`.
The coarse graphics-looking hypothesis contributes another **21,734,012 bytes**
of native text and dispatch records. Thus the candidate-associated text/table
payload is **31,887,756 bytes**, plus pooled linker metadata that is not
region-attributable. This is an accounting ceiling on these payloads, not a
promised file-size delta: the entire graphics-looking interval is not proven
data and must not be applied wholesale on this measurement alone.

The main tail ends two bytes before ROM end: the final checksum word is not FF.
Actual sound samples and tile/sprite pixel ROMs live in separate runtime regions
and were never inputs to discovery. They offer **zero** generated-code savings;
only tables/padding inside the two CPU program images are candidates.

Commands and raw evidence remain ignored under `build/exclude-evidence/`:
`baseline-timing.json`, `baseline-initial-regions-worktree.json`, and
`baseline-accounting-bound.json`. No ROM bytes, generated C or binaries belong
in a commit.
