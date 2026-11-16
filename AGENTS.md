# AGENTS.md

## Hardware observation outranks MAME

MAME: current emulation state of the art, not proof of hardware behaviour. 12: F3 hardware expert; checks against real Taito F3 boards. 12's board-verified observation outranks MAME, datasheet pin labels, and our own ROM/ESP reverse engineering.

- Board observation: fact. MAME behaviour disagreeing with it → emulation bug.
- NEVER argue MAME "already matches" hardware. Don't explain a hardware report away via reinterpretations (e.g. reversed pin numbering, mix-breakage predictions).
- Documentation labels conflict with observation → flag conflict for user or 12; keep hardware conclusion.
- Spend effort on what observed wiring means for emulation and rendering; implement it where user asks.
