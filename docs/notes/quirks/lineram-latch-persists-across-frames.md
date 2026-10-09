---
title: Line RAM latched values persist across frames (notes) vs reset per frame (MAME)
created: 2026-10-09
updated: 2026-10-09
type: quirk
tags: [line-ram, emulator-bug, video]
sources:
  - raw/docs/taito-f3/website/fdp/lineram.html
  - raw/emu-source/mame-0.289/taito_f3_v.cpp
games: []
addresses: []
status: hypothesis
evidence:
  - {kind: doc, ref: raw/docs/taito-f3/website/fdp/lineram.html, note: "'the latched value presists across frames. e.g. if 0 on the first scanline'"}
  - {kind: emu-source, ref: raw/emu-source/mame-0.289/taito_f3_v.cpp, note: "f3_line_inf line_data{} is constructed inside scanline_draw (L1147)"}
contradictions:
  - "Notes: latch state survives the frame boundary. MAME: all latch-held fields start from zero each frame."
supersedes: []
---

# Line RAM latch persistence

Each line register has a per-scanline latch bit. When the bit is 1 the register is loaded from line
RAM for that scanline; when 0 the previous scanline's value is reused ^[raw/docs/taito-f3/website/fdp/lineram.html].

The notes add that the reused value persists across frames, e.g. when a subsection's latch bit is 0
on scanline 0, the value comes from the end of the previous frame ^[raw/docs/taito-f3/website/fdp/lineram.html].

MAME constructs its per-line state (`f3_line_inf line_data{}`) at the top of every `scanline_draw`
call, and `read_line_ram` only overwrites fields whose latch bit is set; fields not latched on
scanline 0 therefore start from their zero/default values every frame
^[raw/emu-source/mame-0.289/taito_f3_v.cpp L1147, L694-L707].

Consequence for a runtime: a game that latches a register only once in a frame, or never on line 0,
would see the previous frame's value on hardware but a default in MAME. Hypothesis: games that write
all 256 lines or latch on line 0 are unaffected. Unverified. ^[raw/docs/taito-f3/line-ram.txt] ^[raw/emu-source/mame-0.289/taito_f3_v.cpp]

See [[comparisons/hardware-vs-mame-video]] and [[hardware/line-ram]].
