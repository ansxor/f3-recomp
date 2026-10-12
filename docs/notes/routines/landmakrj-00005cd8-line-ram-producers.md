---
title: Land Maker video producers by layer (line RAM, tiles, text) and the scenes they drive
created: 2026-10-11
updated: 2026-10-11
type: routine
tags: [video, line-ram, tilemap, text, palette, blend]
sources:
  - raw/emu-source/landmakrj-video.cpp.txt
  - raw/docs/runtime-video-producers.md
  - raw/tests/landmakrj-video-survey-fallbacks.md
  - raw/docs/landmakrj-config-entry-points.toml.txt
games: [landmakrj]
addresses: [0x00005CD8, 0x00005D74, 0x0000136E, 0x0009D66A, 0x0009D72A, 0x0009D7B6, 0x0009ECB0, 0x00091490, 0x00091834, 0x00098DBA, 0x0008CFBA]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/emu-source/landmakrj-video.cpp.txt, note: "known store PCs per layer with the comments of the person who classified them"}
  - {kind: trace, ref: raw/tests/landmakrj-video-survey-fallbacks.md, note: "172,000-frame survey (seeds 5/6/7/41 x 40,000 plus 12,000 no-input attract) labelled effects by producer/field signature; secondhand record, artifacts not retained"}
contradictions: []
supersedes: []
---

# Land Maker video producers by layer

Japan 2.01J. The runtime does not observe producers to render (it decodes video RAM at VBSTART); these
PCs matter to `--discovery-log`, which suppresses a store PC once it is listed as understood.
^[raw/docs/runtime-video-producers.md] They are also the best map of which game routines write which layer
and in which scene. All entries are hypothesis: classification of PCs by the runtime authors, not board
observation. Sprite writers are on [[routines/landmakrj-000041d0-sprite-list-writers]] and
[[routines/landmakrj-000a8e0c-display-object-loop]].

## Known store PCs per layer (video.cpp)

| Layer | PCs | Notes |
|---|---|---|
| Tiles (PF) | `0x55fc 0x5646 0x56d6`, `0x5a2e 0x5a6a 0x5aa6 0x5b00`, `0x9bd08..0x9bd0e`, `0x9ec66 0x9ec6a 0x9ec6e 0x9ec72` | tile-block copy/erase loops and per-layer clear stores |
| Text | `0x570e 0x5712 0x5756 0x5758 0x578e 0x57bc 0x57be 0x580c 0x580e 0x583a 0x5840 0x5846 0x58c2 0x59d0 0x5a08 0x5bf8 0x5c20 0x5b8a 0x5b9a 0x5bb6 0x5bc6 0x5bd8 0x8de60 0x8e0b2 0x8e0c2 0x8e0e6 0x8e0f4 0x8e9d2 0x9b53a 0x9b562 0x9b564 0x9b586 0xa1176..0xa11ac` | map and glyph writers |

^[raw/emu-source/landmakrj-video.cpp.txt] The matching entry seeds (`0x55c2`, `0x5614`, `0x56ae`, `0x5a22`, ...,
`0x8de56`, `0x8e0a6`, `0x8e0dc`, `0x8e9c6`, `0x9b530`, `0xa1170`) are in the proven seed list.
^[raw/docs/landmakrj-config-entry-points.toml.txt]

Line RAM / control-register writers (`lines_covered_write`), by role: ^[raw/emu-source/landmakrj-video.cpp.txt]

| PCs | Role |
|---|---|
| `0x136e..0x145e` | display register uploader (`0x660000..0x66001e`) |
| `0x5a2e..0x5b32` | PF0..PF3 clear rowscroll, zoom, colscroll |
| `0x5d30..0x5d6c` | line RAM profile init (`0x624000..0x62b000`, `0x620000`) |
| `0x10044..0x1007c`, `0x100ba..0x100c4` | boot control registers, boot line-RAM clear |
| `0x8cfd2..0x8cfda`, `0x8cff2..0x8cff8` | alpha blend save / restore (`0x626200`) |
| `0x914b8`, `0x9152e`, `0x915dc..0x91610` | water effect select init / main lines |
| `0x9187a` | water clip window animation |
| `0x9218a` | selection transition sprite priorities |
| `0x98dc8` | full-screen alpha profile (`0x626200`) |
| `0x99b72`, `0x99fc2`, `0x9a25c`, `0x9a2ac`, `0x9a300`, `0x9a6f4`, `0x9a8ec`, `0x9accc`, `0x9ad4c` | attract sprite modes/priorities, reverse/normal blend, alpha fades from task D2, PF1/PF3 blend restore, PF2 blend/priority (`0x62b400`), sprite priorities (`0x627600`), PF2 blending on/off |
| `0x9d684..0x9d6a0`, `0x9d75e..0x9d7aa`, `0x9d7d6..0x9d812` | gameboard PF2 palette-add gradient, zoom and rowscroll, column scroll |
| `0x9ecde..0x9ecee` | PF0 wavy rowscroll (`0x62a000`) |

## Effect producers and the scene each drives (survey catalog)

| Producer | Layer / field | What it does |
|---|---|---|
| `0x5cd8`, ROM profile `0x5d74` | all layer controls, alpha, mosaic, X/Y step | default profile; X step `256 - highByte`, Y step `2 * lowByte`, cross-PF Y mapping `{0,3,2,1}`; default/reset is not an effect |
| `0x136e` | PF/text global scroll | integer/fractional register conversion |
| `0x9d72a` | PF2 X zoom / rowscroll | symmetric about rows 151/152; first steps at both centre rows are 256, then change by 2 per row; 8-bit wrap; centring uses division/remainder at `0x9d774..0x9d7aa` |
| `0x9d7b6` | PF2/PF3 column offset | phase `$40790a & 127` after a task wake; two constant PF2 halves at row 152 and PF3 rows 0-47 from the uploader spill |
| `0x9ecb0` | PF0 sine rowscroll | signed-byte phase into the 32-bit ROM table `0x1c84`, low-word negation, shift/swap, phase +2 per line; wraps at the 1024-texel map period |
| `0x9d66a`, table `0x9d6a8..0x9d6d8` | PF2 palette add | 11 held bands of 4/4/4/6/8/8/8/12/16/16/18 rows, offsets 640 down to 0 by 64 |
| `0x91490 0x91506 0x915d2` | text / PF3 / sprite priority, clip, blend | rectangular selection-label and water row blocks; PF3 water rows 176-251, fixed alpha `{5,4,3,4}` |
| `0x91834` | clip plane 3 | held edge across rows 176-251, expanding by 6 per frame |
| `0x98dba..0x9ad3e`, `0x8cfba`/`0x8cfe0` | alpha, mix, sprite priorities | uniform row blocks, frame-time fades, saved-alpha restore |

^[raw/tests/landmakrj-video-survey-fallbacks.md] ^[raw/docs/runtime-video-producers.md]

- The animated water/star backdrop during character selection is **PF2** (`0x9d66a/0x9d72a/0x9d7b6`, X step
  2-256); the played board's diamond ground texture is **PF0**, pre-drawn perspective in ROM tiles with
  X/Y steps 256 (no per-line zoom). Older "water/puzzle-board floor" naming conflated the two.
  ^[raw/docs/runtime-video-producers.md]
- Survey frequencies: PF0 sine source-X varied in 1,352 of 172,000 frames, PF2 X-step/source-X/palette in
  4,381, alpha row blocks in 5,458; PF1/PF3 had no varying source-X, step or palette-add; no active
  mosaic and no vertical zoom ramp (all enabled Y steps 256) were seen. ^[raw/tests/landmakrj-video-survey-fallbacks.md]
- The words `0x401f54`/`0x401f5c` were recorded as raw mode/phase values (2/3/4/7 in played matches, `$ffff` in
  no-input demos), not as a scene enum; see [[routines/landmakrj-0008e20c-round-task]].
  ^[raw/tests/landmakrj-video-survey-fallbacks.md]

## Ending and fallbacks

`0xfe620`, `0xfefe6`, `0xff0fa` are the ending bitmap/slide producers; they were audited as unsupported by
the semantic line decoder (exact FDP-oracle fallback) and **never exercised by a played ending**.
^[raw/docs/runtime-video-producers.md] The three oracle-fallback triggers are sprite command bit 13
(`flipped-screen`), sprite command bit 1 (`sprite-trails`) and pivot control `$a0` bits on a visible row
(`bitmap-pivot`). In the 172,000-frame survey none appeared in play: the 1,155 unsupported frames were all
startup oracle frames (231 per route), and the three kinds were only induced artificially at frame 3404.
^[raw/tests/landmakrj-video-survey-fallbacks.md] Which boot condition causes the 231 startup frames is not
stated in the sources.

Related: [[hardware/line-ram]], [[hardware/tilemaps]], [[hardware/fda]], [[games/landmakrj]].
