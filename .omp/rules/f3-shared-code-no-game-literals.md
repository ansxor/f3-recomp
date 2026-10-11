---
name: f3-shared-code-no-game-literals
description: "Shared F3 runtime edits must not hardcode a specific game; use a config.toml opt-in or a public per-game interface"
condition: ["\\[(?![^\\]]*games/)[^\\]#]*\\.(cpp|cc|h|hpp|c)#[0-9A-Fa-f]{4}\\][\\s\\S]*?(?i:landmak|kaiserkn|bubsymph|pbobble|gseeker|arabianm|dariusg|elvactr|cupfin|ringrage|spcinvdj|trstar|puchi|recalh|lightbr|gunlock)", "\"path\"\\s*:\\s*\"(?![^\"]*games/)[^\"]*\\.(cpp|cc|h|hpp|c)\"[\\s\\S]*?(?i:landmak|kaiserkn|bubsymph|pbobble|gseeker|arabianm|dariusg|elvactr|cupfin|ringrage|spcinvdj|trstar|puchi|recalh|lightbr|gunlock)"]
scope: "tool"
---

Shared runtime code outside `games/<game>/` MUST NOT name a game or romset, branch on which game is running, or hardcode one game's addresses, hooks, timings, or quirks.

Pick one of these instead:

1. **Config opt-in (data):** add a generic, named setting to `config.toml` that any game can enable, such as an address, flag, or timing. The shared code reads the setting and never checks which game it is running.
2. **Public per-game interface (logic):** declare a public interface that shared code calls. Each game supplies its implementation under `games/<game>/...`, following the pattern of `games/landmakrj/netplay/netplay.cpp` and `games/landmakrj/boot/boot.cpp`.

If the difference is data, use the config opt-in. If it is code, use the per-game interface. Name the choice in your design before you edit.