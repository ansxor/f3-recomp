## Land Maker sprite-grouping report (static only)

No runtime run and no repo edits. The `f3rt-tool` binary is not on PATH and I was not allowed to build it, so everything below is from f3a disassembly, xref, and the existing docs. `[INFERENCE]` marks anything not directly observed.

### Q1. Queue push sites and records

**Queue and record layout (observed)**
- Eight buckets. Drain entries are at 0x4542…0x4668. Each bucket has a counter at `a5-0x5c2…-0x5ca` and a write cursor (cell) at `a5-0x5a4…-0x5b4`.
- Push idiom (sub_08ea20 at 0x8ea20..0x8eaee): seven `move.w …,-(a0)`, then `move.l T,-(a0)`, then `subq.w #1,cnt(a5)`, then `move.l a0,cell(a5)`. The record is 18 bytes and the cell points at the newest record.
- Record fields (from the drain and handlers): +0 long template pointer T; +4 zoom/flags; +8 X (12-bit); +10 Y (12-bit); +12/+14/+16 attribute words C, B, A.
- Drain: `movea.l (a4)+,a0; move.l (a0)+,d7`. Zero template selects the single handler (0x4688, one sprite entry). Non-zero selects the grid handler (0x46c0).
- Grid handler (0x46c0..0x471e, observed): outer `dbra d7` over rows; inner `dbra d1` over columns; column step `d4=0x10`; each cell reads one tile long via `(a0)+`, skips zero, and writes four words to sprite RAM. So one grid record can expand into several sprite entries that share one X/Y base. `[INFERENCE]` on how `d7`/`d1` are loaded from the template.

**Push-site sampling (static, 113 sites found vs the config's "~136")**
- sub_08ea20 (0x8ea5e, 0x8eaa0, 0x8eae2): 13 records from ROM tables via `(a1)+`, `dbra d5`. Positions are ROM offsets, not RAM.
- sub_093080 (0x930da..0x93316): 12 fixed-position records, T in ROM range 0x2f8b8..0x2f8c0, X 0x78..0xd0, Y ∈ {0x40, 0x50, 0x80}, C=0x7c.
- sub_09d9f4 (11 sites): mostly immediates, including a 3×3 layout at X 0x74/0x90/0xac, Y 0x64/0x80/0x9c.
- sub_08f00a (0x8f03a): T=#$30014, X=#$18, Y=#$38, C=#$d4. Fixed.
- sub_09b5aa (0x9b5d8): reads the object struct at a2: T=`$14(a2)`, X=`$4(a2)`, Y=`$8(a2)`, C=`$20(a2)`, B=`$22(a2)`, A=`$24(a2)`. O-driven.
- sub_094c14 (0x94c5c): object struct at a2. X=`$4(a2) - ((w+1)<<3)`, Y=`$8(a2) - ((h+1)<<4)`, where w/h come from the size struct at `$1c(a2)` (`(a0)` and `2(a0)`). T=`$1c(a2)` (pointer to the size struct), C=`$20(a2)`. O-driven, and the size struct sets the grid dimensions. `[INFERENCE]` that the grid dimensions come from this struct.
- sub_096cc0 (0x96d24): object `a3`. X=`$4(a3)+0x10`, Y=`$8(a3)-6`, C=`($26(a3)&0xf)+0x10`, T=`$60(a3)`. T is a frame template advanced by a per-frame timer `$5a(a3)` and frame list `$5c(a3)`. O-driven, animated.
- Most other sites take X/Y from immediates, ROM tables, or simple `addq`/`add` on registers. The register base for those is not traced.

**Answers to Q1**
- (a) Multi-record-per-object pushes: no evidence found for a loop over objects that pushes several records per object. Object-driven routines push one record per call (sub_09b5aa, sub_094c14, sub_096cc0). Multi-record calls exist only for fixed or ROM-table layouts (sub_08ea20: 13; sub_093080: 12; sub_09d9f4: 11). `[INFERENCE]` that the per-frame cadence is one record per object per frame.
- Records from object-driven pushers carry O positions (`$4`/`$8` of the object). Records from fixed/ROM sites carry no O.
- Grid records (template+0 non-zero) fan out into multiple sprite entries sharing one base position, so one record can be an SG by itself. Single records yield exactly one entry (observed in the single handler).
- Queue content: `[INFERENCE]` mostly fixed or static content (ROM-positioned tile grids and fixed overlays), with object-driven records a minority. Not verified as "background" specifically.

**Counter reset:** the reset write was not found. The counters have no `write` xref rows at their absolute offsets. Unresolved.

### Q2. Title and player-select cards

**Title (candidate, `[INFERENCE]` from call chain and position data):**
- Chain: `sub_08fc54` → `sub_090d48` (attract demo, call at 0x08fc7a) → `sub_090f08` (0x090d94) → `sub_092f30` (0x090f1c) → `sub_093080` (0x092f52).
- `sub_093080` draws 12 fixed records (X 0x78..0xd0, Y 0x40..0x80), consistent with a title logo. No O is involved (immediates), so no SG through a shared O.

**Player-select cards:**
- Chain: `sub_08ee06` (mid-charsel join, ref at 0x08ee0c) or `sub_08f79a` (challenger banner, ref at 0x08f872) → `sub_09cd04` (character select task, per `docs/notes/routines/landmakrj-0008edde-versus-mode-transition.md`) → `sub_09d6d2` (dispatcher; calls at 0x09d6da, 0x09d6e2, 0x09d6e6, 0x09d6ea) → `sub_09d9f4` (11 fixed records, 3×3 grid), `sub_09de96`, `sub_09e0f8`, `sub_09f16a`.
- These card records are fixed immediates, so they are not an SG via a shared O.
- Object-driven pushers are reached via slot tables: sub_094bb6 is a slot entry (0x0039ae, 0x003d7a, 0x0953f6, 0x099bea, …); sub_09b5aa is called from sub_09b636, sub_09b704, sub_09b790, sub_09ba4a, sub_09bbd4.

**Answers to Q2**
- (b) Title and card sprites through a shared O: the title (`sub_093080`) and card layout (`sub_09d9f4`) use fixed immediates, so they do not form an SG through a shared O. The O-driven SG candidates are sub_094c14 (size-struct grid on object `a2`) and sub_096cc0 (animated object `a3`). Neither has been matched to a card or title sprite. `[INFERENCE]` that sub_094c14 is the card SG, since its size-struct grid fits a multi-tile portrait; not verified.
- Gap: I did not determine the object type behind the slot entry sub_094bb6, so I cannot yet name the O address or the object struct for the card SG. Map the slot table entry to its object type to finish this.

### Next steps for Main
1. Confirm the title/card identity at runtime (e.g., `f3rt-tool sprite-check` after building) if you want to replace the `[INFERENCE]` marks.
2. Map the slot entry for sub_094bb6 (and the sub_09b636 family) to its object type to name the O struct for card SGs.
3. Resolve the counter reset site (indexed or bulk-clear writes near `a5-0x5c2…-0x5ca`).