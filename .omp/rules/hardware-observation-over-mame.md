---
name: hardware-observation-over-mame
description: "Never treat MAME as ground truth against 12's direct Taito F3 PCB observations"
condition: "(?i)\\bMAME\\b[^.\\n]{0,120}\\b(already matches|matches hardware|correct|accurate|authoritative|contradicts?|refutes?|disagrees?|would break|ground truth)|\\b(hardware|board|PCB|12)\\b[^.\\n]{0,120}\\b(contradicts?|disagrees? with|conflicts? with)\\b[^.\\n]{0,40}\\bMAME\\b"
question: "Does this output treat MAME (or datasheets or ROM/ESP microprogram analysis) as evidence that can override, discount, reinterpret away or argue against a hardware observation 12 made on a real Taito F3 PCB?"
scope: ["text", "thinking"]
---

MAME: current emulation state of the art, not proof of hardware behaviour. 12: F3 hardware expert; checks against real Taito F3 boards. 12's board-verified observation outranks MAME, datasheet pin labels, and our own ROM/ESP reverse engineering.

- Board observation: fact. MAME behaviour disagreeing with it → emulation bug.
- NEVER argue MAME "already matches" hardware. Don't explain a hardware report away via reinterpretations (e.g. reversed pin numbering, mix-breakage predictions).
- Documentation labels conflict with observation → flag conflict for user or 12; keep hardware conclusion.
- Spend effort on what observed wiring means for emulation and rendering; implement it where user asks.
