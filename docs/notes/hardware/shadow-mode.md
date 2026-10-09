---
title: Shadow mode (palette-bit modification between neighbouring priorities)
created: 2026-10-09
updated: 2026-10-09
type: hardware
tags: [video, blend, palette, line-ram, undocumented]
sources: [raw/docs/taito-f3/website/fdp/shadow.html, raw/docs/taito-f3/shadow.txt, raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/website/fdp/priority.html, raw/emu-source/mame-0.289/taito_f3_v.cpp]
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/shadow.html, note: "hardware-tested table of palette bits changed per bpp and shadow mode"}
  - {kind: doc, ref: raw/docs/taito-f3/shadow.txt, note: "raw test captures behind the table"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "no shadow handling found in taito_f3_v.cpp (searched 'shadow')"}
contradictions:
  - "shadow.txt first says sprite shadow mode 2 at bpp=2 has no effect, then says bpp=2 'DOES have an effect' on sprites and tilemaps (probably the bit-5 bitplane); the website table says no effect for shadow=2 at bpp=2 but bits 5-12 cleared for shadow=3."
  - "line-ram.txt describes shadow as setting only 'the lower bit' of the palette (with a todo about bpp); the website table shows different bits per bpp and a bit-clearing mode 3."
  - "MAME has no shadow implementation; 12Me21 documents it as hardware behaviour."
supersedes: []
---

# Shadow mode (palette-bit modification between neighbouring priorities)

When two layers have neighbouring priority values within a half-pixel column and both have non-transparent
pixels at the same screen position, the upper layer's color id is altered. The note stresses: "this is
the only situation where a pixel can have an effect on the output without being the highest priority in
its halfpixel". ^[raw/docs/taito-f3/website/fdp/shadow.html] It is probably implemented inside the priority
cell array ([[hardware/priority-and-blend]]). ^[raw/docs/taito-f3/website/fdp/priority.html]

The background color layer and priority conflicts count as empty for this purpose. ^[raw/docs/taito-f3/line-ram.txt]
Because conflicts matter, a conflicting layer can "cut a hole" in the shadow effect (e.g. shadow a tilemap onto
another tilemap, then use conflicting sprites to cut a hole in the lower tilemap). ^[raw/docs/taito-f3/website/fdp/shadow.html]

## Control

Line register 2.2 ("6400") bits 11–10 `sh`: 0 or 1 = off, 2 = shadow mode 2, 3 = shadow mode 3 (never used by
games). ^[raw/docs/taito-f3/website/fdp/shadow.html] ^[raw/docs/taito-f3/website/fdp/lineram.html] The older
line-ram.txt calls bit 11 "S" (shadow mode) and bit 10 "g" ("changes the behavior of shadow mode somehow").
^[raw/docs/taito-f3/line-ram.txt]

## Effect on the upper layer's color bits

Bit numbers refer to the 13-bit color id (texture color in bits 0–5, palette bits above 4; 'sprites normally
have color bit 12 hardcoded to 1'). ^[raw/docs/taito-f3/website/fdp/shadow.html]

| Upper layer | shadow=2 | shadow=3 |
|---|---|---|
| pivot | bit 4 = 1 | bits 6–11 = 0, bit 12 = 1 (!) |
| bpp=0 (4-bit) | bit 4 = 1 | bits 6–12 = 0 |
| bpp=1 (5-bit) | bit 5 = 1 | bits 4, 6–12 = 0 |
| bpp=2 | no effect | bits 5–12 = 0 |
| bpp=3 (6-bit) | bit 6 = 1 | bits 4–12 = 0 |

^[raw/docs/taito-f3/website/fdp/shadow.html]

Bit-level layout, `t` = texture bit n, `p` = palette bit (n−4), `Q` = palette bit OR texture bit, MSB left: ^[raw/docs/taito-f3/website/fdp/shadow.html]

```
bpp=0  off [p pppp pppp tttt]  m2 [p pppp ppp1 tttt]  m3 [0 0000 00pp tttt]
bpp=1  off [p pppp pppQ tttt]  m2 [p pppp pp1Q tttt]  m3 [0 0000 00p0 tttt]
bpp=2  off [p pppp ppQp tttt]  m2 same as off         m3 [0 0000 000p tttt]
bpp=3  off [p pppp ppQQ tttt]  m2 [p pppp p1QQ tttt]  m3 [0 0000 0000 tttt]
pivot  off [0 00pp pppp tttt]  m2 [0 00pp ppp1 tttt]  m3 [1 0000 0000 tttt]
```

Other findings in shadow.txt: ^[raw/docs/taito-f3/shadow.txt]

- bpp=2 with sprites does have an effect: it seems to enable only the bit-5 bitplane, and bpp=3 is the
  combination of the two. ^[raw/docs/taito-f3/shadow.txt]
- The bpp setting also clears low palette bits: palette 3 with bpp=0/1/2/3 gives palette 3/2/1/0. ^[raw/docs/taito-f3/shadow.txt]
- Captured values (shown with the low bits as `....`): shadow on: bpp=0 $1C, bpp=1 $3C, bpp=2 $2C,
  bpp=3 $7C; shadow off: $0C, $1C, $2C, $3C. In the same file the early line "bpp=2 ... no effect" for sprites
  is later contradicted by the note that bpp=2 does have an effect. ^[raw/docs/taito-f3/shadow.txt]

## Which layers can shadow onto which

"There isn't any clear rule." Table of tested combinations (top layer in rows, bottom in columns; blank =
not shown as working / untested): ^[raw/docs/taito-f3/website/fdp/shadow.html]

| top \ bottom | tm0 | tm1 | tm2 | tm3 | sprite | pivot | highcolor |
|---|---|---|---|---|---|---|---|
| tm0 | – | | | | | | |
| tm1 | yes | – | | | | | |
| tm2 | yes | | – | | | | |
| tm3 | yes | yes | yes | – | | yes | yes |
| sprite | yes | | | | – | | |
| pivot | yes | | yes | | yes | – | |
| highcolor | yes | yes | | | yes | | – |

The test for highcolor is still a todo. ^[raw/docs/taito-f3/website/fdp/shadow.html]

## MAME

taito_f3_v.cpp contains no shadow-mode handling (its comments describe only 0x6400's mosaic bits and blur/palette-format
bits). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] Hypothesis: a recompiled renderer that follows MAME will
render shadowed pixels with the unmodified palette; see also [[comparisons/hardware-vs-mame-video]] and
[[quirks/shadow-mode-3-pivot-sets-bit12]].
