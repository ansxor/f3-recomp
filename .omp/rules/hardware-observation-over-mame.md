---
name: hardware-observation-over-mame
description: "Never treat MAME as ground truth against 12's direct Taito F3 PCB observations"
condition: "(?i)\\bMAME\\b[^.\\n]{0,120}\\b(already matches|matches hardware|correct|accurate|authoritative|contradicts?|refutes?|disagrees?|would break|ground truth)|\\b(hardware|board|PCB|12)\\b[^.\\n]{0,120}\\b(contradicts?|disagrees? with|conflicts? with)\\b[^.\\n]{0,40}\\bMAME\\b"
question: "Does this output treat MAME (or datasheets or ROM/ESP microprogram analysis) as evidence that can override, discount, reinterpret away or argue against a hardware observation 12 made on a real Taito F3 PCB?"
scope: ["text", "thinking"]
---

MAME is the current state of the art in emulation, not proof of how the hardware behaves. 12 is the F3 hardware expert and checks things against real Taito F3 boards. When 12 reports something verified on the board, that observation outranks MAME, datasheet pin labels and our own ROM/ESP reverse engineering.

- Treat the board observation as fact, and treat MAME behaviour that disagrees with it as an emulation bug.
- NEVER argue that MAME "already matches" the hardware. Don't use reinterpretations such as reversed pin numbering or mix-breakage predictions to explain a hardware report away.
- If documentation labels conflict with the observation, flag the conflict for the user or 12. Keep the hardware conclusion.
- Spend the effort on what the observed wiring means for emulation and rendering, and implement it where the user asks.