---
title: Clip windows and mosaic
created: 2026-10-09
updated: 2026-11-18
type: hardware
tags: [video, clip, line-ram]
sources: [raw/docs/taito-f3/website/fdp/clip.html, raw/docs/taito-f3/clip.txt, raw/docs/taito-f3/website/fdp/mosaic.html, raw/docs/taito-f3/website/fdp/layer.html, raw/docs/taito-f3/website/fdp/lineram.html, raw/docs/taito-f3/line-ram.txt, raw/docs/taito-f3/graphics.txt, raw/docs/taito-f3/terms.txt, raw/emu-source/mame-0.289/taito_f3_v.cpp, raw/emu-source/mame-0.289/taito_f3.h, raw/docs/fdp-clip-12-remark-2026-11-18.txt, raw/tests/2026-11-18-calc-clip-y-ack-branches.md]
games: []
addresses: [0x00620000]
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/clip.html, note: "window geometry and per-layer clip bits"}
  - {kind: doc, ref: raw/docs/taito-f3/clip.txt, note: "author's scratch algorithm for combining windows (not hardware)"}
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/mosaic.html, note: "mosaic sampling description"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "calc_clip() and mosaic() implementations"}
  - {kind: doc, ref: raw/docs/fdp-clip-12-remark-2026-11-18.txt, note: "12 (2026-11-18): worked out the real clip behaviour on hardware; the website page describes it"}
  - {kind: test, ref: raw/tests/2026-11-18-calc-clip-y-ack-branches.md, note: "every y-ack/mame branch with calc_clip has the same rule as MAME 0.289; our FDP oracle and GameVideo port it"}
contradictions:
  - "Clip enable polarity: website/fdp/clip.html says c_n 0 = enabled, 1 = disabled; line-ram.txt ('1 = enable clip plane') and MAME ('If set, enable corresponding clip plane') say 1 = enabled. Resolved 2026-11-18: 12 confirmed clip.html's 0 = enabled is a typo; 1 = enabled, 0 = disabled."
  - "Multi-window combination: clip.html says the visible parts of the selected windows are unioned; MAME intersects the enabled windows (with inverted windows as complements), and clip.txt's scratch code uses a union-style nonzero mask. Resolved 2026-11-18: 12 points to clip.html as the board-verified rule, so union and MAME is wrong."
  - "'Global invert' I: clip.html/line-ram.txt say I=1 inverts; MAME's comment says 'if on, 1 = invert; if off, 0 = invert' and swaps the plane modes when I=0."
  - "No windows enabled: clip.txt's layer_calc_ranges only makes the layer visible when invert_all is set; MAME leaves the whole line visible. Resolved 2026-11-18: 12 confirmed hidden with I=0, fully visible with I=1; MAME is wrong."
  - "Bit 9 of line register 2.2: 12Me21 says highcolor-layer mosaic enable (line-ram.txt: 'NOT pivot mosaic'); MAME uses it as pivot-layer mosaic enable."
supersedes: []
---

# Clip windows and mosaic

## Inverted windows: 12's rule (2026-11-18)

12 worked out the real behaviour on a board and points to the website page as the description of it
("maybe not in particularly exact detail"). ^[raw/docs/fdp-clip-12-remark-2026-11-18.txt] ^[raw/docs/taito-f3/website/fdp/clip.html]
So the hardware rule is the website's: each enabled window contributes its visible part (inside if `i`=1,
outside if `i`=0), the visible parts are **unioned**, and `I`=1 inverts the result.

Enable polarity: clip.html says `c`=0 is enabled, but 12 confirmed on 2026-11-18 that this is a typo and
**`c`=1 is enabled**, adding "this one is correct in mame" (line-ram.txt agrees). ^[raw/docs/fdp-clip-12-remark-2026-11-18.txt] In raw bits, the hardware rule per line and layer is:

```
visible(x) = I XOR OR over n with c[n] == 1 of ( i[n] ? inside(n, x) : outside(n, x) )
inside(n, x) = (left[n] - 47) <= x < (right[n] - 48)      # empty window: inside never true
```

With `c`=0000 (no windows enabled) the union is empty, so the layer is hidden when `I`=0 and fully shown when `I`=1
(clip.txt's scratch code agrees; 12 confirmed this case directly on 2026-11-18). ^[raw/docs/taito-f3/website/fdp/clip.html] ^[raw/docs/taito-f3/clip.txt] ^[raw/docs/fdp-clip-12-remark-2026-11-18.txt]
12: games commonly "disable" clipping this way, with all windows off (`c`=0000) and `I`=1, so the whole line is visible.
^[raw/docs/fdp-clip-12-remark-2026-11-18.txt]

MAME 0.289's `calc_clip`, and both our renderers, which port it, use the same enable polarity but differ in two ways: ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] ^[raw/tests/2026-11-18-calc-clip-y-ack-branches.md]

1. **No windows enabled:** MAME shows the whole line whatever `I` is; on hardware `c`=0000 with `I`=0 hides the layer.
2. **Combination:** MAME intersects windows (and with `I`=0 it swaps the meaning of `i`). That equals the
   website's union-then-invert only for one window or `I`=1; with `I`=0 and two or more windows, MAME
   intersects where the hardware unions.

The y-ack/mame fork 12 mentioned has the same `calc_clip` as 0.289 in every branch (including
`fdp-collapse`), so it does not contain the website's rule. ^[raw/tests/2026-11-18-calc-clip-y-ack-branches.md]
Not yet measured: which games and lines change under the hardware rule. Screenshots before and after the change are needed
so 12 can check them.

Both effects are per-scanline settings in [[hardware/line-ram]]. Clip bits live in each layer's mix word
(see [[hardware/priority-and-blend]]); mosaic is a global strength plus per-layer enables in line
register 2.2. The layers affected are described on [[hardware/tilemaps]], [[hardware/sprites]] and
[[hardware/pivot-layer]].

## The four clip windows

There are four clip windows ("planes", "intervals"); each has a left and right value of 0–511, equal to
the screen interval `[left − 47, right − 48)` in pixels. Example: left=47, right=50 → `[0,2)`, the first
two columns. ^[raw/docs/taito-f3/website/fdp/clip.html]

MAME's calc_clip uses `l − 1` and `r − 2` in its own coordinates where the visible area starts at x=46, which
equals the same `[left − 47, right − 48)` interval. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp] ^[raw/emu-source/mame-0.289/taito_f3.h]

Values are split across two line registers (each one a list of 256 per-scanline words, see [[hardware/line-ram]]): ^[raw/docs/taito-f3/website/fdp/clip.html] ^[raw/docs/taito-f3/line-ram.txt]

| Register | Bits | Meaning |
|---|---|---|
| 0.2 "4400" | 15 / 14 / 13 / 12 | window 1 right high bit / window 1 left high bit / window 0 right high bit / window 0 left high bit |
| 0.3 "4600" | same | same, for windows 3 and 2 |
| 1.N "5000"+N·0x200 (N = window 0–3) | 15–8 / 7–0 | right low 8 bits / left low 8 bits |

(The low bits of 0.2/0.3 hold other data: tilemap 2/3 y-scroll adjust and the alt-tilemap flag.)
^[raw/docs/taito-f3/website/fdp/clip.html] MAME reads the same bit assignments (bits 12–15 of 4400/4600, low
bytes from 5000–5600). ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Windows with left ≥ right are skipped by clip.txt's code ("invalid(?) interval (start > end)"); MAME clips the
whole line to nothing for an enabled normal window with `l − 1 > r − 2`, and ignores such a window when inverted.
^[raw/docs/taito-f3/clip.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

## Per-layer subscription

Each clip-capable layer has these bits in its mix word (bits 13–4 of the 16-bit word; bits 3–0 are
priority, 15–14 blend mode): ^[raw/docs/taito-f3/website/fdp/clip.html] ^[raw/docs/taito-f3/line-ram.txt]

| Bit | Name | Meaning |
|---|---|---|
| 13 | E | layer enable (1 = displayed) |
| 12 | I | global invert of the clip result (1 = inverted) |
| 11–8 | c3..c0 | window n enable: 1 = enabled (12, 2026-11-18; clip.html's "0 = enabled" is a typo) |
| 7–4 | i3..i0 | window n mode: 0 = outside visible ("hide inside"), 1 = inside visible ("hide outside") |

MAME's header names these bits `BAEI cccc iiii pppp` for B,A = blend bits, E = enable line, I = affects
interpretation of the invert bits; its code tests `mix_value & 0x2000` for E. ^[raw/emu-source/mame-0.289/taito_f3.h] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Mix word locations: tilemaps 7.0–7.3 ("B000"–"B600"), pivot 3.1 ("7200"), highcolor 3.0 ("7000"), sprites
3.2 ("7400", shared by all four groups, with 10 bits = `E I cccc iiii`). ^[raw/docs/taito-f3/line-ram.txt]
The glossary says a layer "subscribes to" a clip interval when that interval is enabled for it. ^[raw/docs/taito-f3/terms.txt]
The background color layer cannot be clipped. ^[raw/docs/taito-f3/graphics.txt]

### How windows combine

- Website: "Each layer can use any combination of these windows or their complements, and the visible parts
  are unioned together to produce a final mask (which can then be inverted as well)". ^[raw/docs/taito-f3/website/fdp/clip.html]
- clip.txt (scratch pseudo-code, not a hardware description): compute per
  scanline a `basemask` (windows that cover x=0) and a sorted list of edge points; per layer start with
  `mask = (basemask ^ invert_mask) & enable_mask`, toggle bits at each edge, and emit a visible range
  while `mask != 0`; `clip_invert_all` complements the mask first (and with no windows enabled the layer is
  visible only if invert_all is set). ^[raw/docs/taito-f3/clip.txt]
- MAME: starts with the full line and, for each window, intersects normal windows and carves out inverted
  windows (it assumes "only up to two clip settings legal at a time"). When I = 0 it swaps which planes
  count as normal vs inverted. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
- Hypothesis: for layers using a single window the three descriptions agree; multi-window behaviour is
  not settled by these sources.

clip.txt itself says its first attempt was "probably wrong" and switches to a bitmask version; it is a
scratch algorithm for an emulator, not a hardware description. ^[raw/docs/taito-f3/clip.txt]

## Mosaic

A horizontal sample-and-hold ("pixelation") filter. The sampling interval is 1–16 pixels; it is enabled per
layer and works on all layers except pivot (according to the website; 12Me21 suspects the pivot port is used
instead). ^[raw/docs/taito-f3/website/fdp/mosaic.html]

Line register 2.2 ("6400") ^[raw/docs/taito-f3/website/fdp/lineram.html] ^[raw/docs/taito-f3/line-ram.txt]:

| Bits | Meaning |
|---|---|
| 7–4 `mosaic` | samples every `16 − mosaic` pixels: $0 = 16 px, $E = 2 px, $F = 1 px (= off) |
| 3–0 | mosaic enable for tilemaps 3..0 (bit n = tilemap n) |
| 8 | sprite layer mosaic enable (all four groups) |
| 9 | highcolor-layer mosaic enable (12Me21; MAME treats bit 9 as pivot mosaic) |

MAME decodes the same fields: `x_sample = 16 − BIT(v,4,4)`, enable bits 0–3 per playfield, bit 8 for all
sprite groups, bit 9 for the pivot layer; its header comment says "x repeat 0 = repeat 1st pixel 16 times
… f = sample every pixel". ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

Sampling columns are fixed and the effect resets 2 pixels before the right edge of the screen; the author
leaves open which line's parameters apply at that edge. ^[raw/docs/taito-f3/website/fdp/mosaic.html]
MAME's rule: `x_count = screen_x + 114`, wrapped by subtracting 432 when ≥ 432, and the sampled column is
`x − (x_count % interval)`; see [[quirks/mosaic-counter-resets-before-right-edge]]. ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]
Hypothesis: because the counter starts at 114 at screen column 0, sampling phase depends on the interval
(not simply on column 0), and it restarts from 0 at column 318.
