---
name: f3-address-constexpr
description: "F3 addresses, ROM PCs and offsets in C/C++ must be descriptively named constexpr constants, never raw hex literals"
condition: "(?m)^(?![ \\t]*(?://|/?\\*))(?!.*\\bconstexpr\\b).*(?:\\b0x[0-9A-Fa-f]{3,}\\b|[+-][ \\t]*0x[0-9A-Fa-f]+\\b)"
question: "Does this C/C++ code use a raw hex literal as an F3 memory/bus address, ROM PC, or RAM/register/struct offset anywhere other than the initializer of a descriptively named constexpr constant? Answer no for hex literals that are bit masks, sizes, counts, colours, opcodes or other non-address data."
scope: ["tool:edit(*.cpp)", "tool:edit(*.hpp)", "tool:edit(*.h)", "tool:edit(*.c)", "tool:write(*.cpp)", "tool:write(*.hpp)", "tool:write(*.h)", "tool:write(*.c)"]
---

Every F3 address, ROM PC or offset in runtime or per-game code, such as `pc==0xc130f0`, `address==0x20001e`, `A4+0x14` or `0xd81a`, MUST be a `constexpr` with a descriptive name, declared once and referenced by name. Masks, sizes, counts and other non-address data are out of scope.

- **Board hardware:** put it next to the existing board constants (`SHARED_BASE`, `SOUND_RESET_ASSERT`). Examples: `constexpr uint32_t OTIS_PAGE = 0x20001e;`, `SOUND_TRACE_BUS_BEGIN`/`SOUND_TRACE_BUS_END`.
- **Game-specific ROM PCs and work-RAM offsets:** put them in that game's namespace, keyed by ROM CRC. Examples: `constexpr uint32_t LANDMAKR_SND_PC_DIRECT_NOTE = 0xc130f0;`, `constexpr uint32_t VOICE_OWNER_OFFSET = 0x0a;`.
- **Shared values:** if the interpreter and native paths use the same value, define it once in a shared header.
- **Example:** `if (pc == LANDMAKR_SND_PC_DIRECT_NOTE)`, not `if (pc == 0xc130f0)`.