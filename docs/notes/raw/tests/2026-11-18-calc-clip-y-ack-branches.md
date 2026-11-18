---
ingested: 2026-11-18
kind: source comparison
---
# calc_clip across y-ack/mame branches vs MAME 0.289

Fetched `src/mame/taito/taito_f3_v.cpp` (or `tc0630fdp.cpp` once the FDP became a device) from every
branch of https://github.com/y-ack/mame on 2026-11-18 and diffed the `calc_clip` function against
`raw/emu-source/mame-0.289/taito_f3_v.cpp` (whitespace ignored).

| Branch | Head commit date | calc_clip vs 0.289 |
|---|---|---|
| fdp-collapse (head 28e411d4f7) | 2026-10-01 | identical except the class name (`tc0630fdp_device::`) |
| fdp-device-merge (tc0630fdp.cpp) | 2025-07-25 | identical except the class name |
| perf-merge | 2024-05-02 | identical |
| performance, f3_rewrite | 2024-04 | same algorithm; older iterator handling and naming only |
| twelve, thirteen, blending_2, extra_subsections | 2024-01 | same algorithm; `INF_L = 46`, `INF_R = 366` literals, `const mixable *` parameter |
| f3_zoom_fix, pfmode, tilemaps_tonite, f3_sicut_y_scivit_eam | 2023 | no `calc_clip` (the older driver structure) |

Clip register decoding (`set_upper` from line RAM colscroll bits 12–15, `set_lower` from the clip word,
mix-word bits 4–7 invert, 8–11 enable, 12 invert mode) is the same in fdp-collapse, MAME 0.289 and our
FDP oracle (`runtime/renderer/fdp/video.cpp` `calc_clip`). GameVideo's
`runtime/renderer/game/clip.hpp` `clip_ranges` is a fixed-array port of the same algorithm, including
the `std::max(range.l, ...)` on both ends of each inverted-window candidate.

No branch in the fork has a different inverted-window rule from 0.289, so the rewrite (first seen in the
January 2024 branches) is the newest version in the fork, and it is what upstream MAME 0.289 ships.
