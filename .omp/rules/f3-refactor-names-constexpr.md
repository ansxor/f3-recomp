---
name: f3-refactor-names-constexpr
description: "When proposing cleanup of hard-coded F3 addresses/PCs/offsets, specify descriptively named constexpr constants, not a vague table"
condition: "(?i)(probe table|cleaner design|refactor|in one place)"
question: "Does the reply recommend a cleanup, refactor or redesign of hard-coded hex addresses, ROM PCs or RAM offsets without explicitly saying that each value becomes its own descriptively named constexpr constant?"
scope: "text"
---

When you find magic hex values in F3 runtime or per-game code, such as `pc==0xc130f0`, `address==0x20001e`, `A4+0x14` or `0xd81a`, the fix you propose (or write) MUST turn each one into a `constexpr` with a descriptive name. A vague "probe table" or "put the constants in one place" is not enough.

- **Board hardware:** put it next to the existing board constants (`SHARED_BASE`, `SOUND_RESET_ASSERT`). Examples: `constexpr uint32_t OTIS_PAGE = 0x20001e;`, `SOUND_TRACE_BUS_BEGIN`/`SOUND_TRACE_BUS_END`.
- **Game-specific ROM PCs and work-RAM offsets:** put them in that game's namespace, keyed by ROM CRC. Examples: `constexpr uint32_t LANDMAKR_SND_PC_DIRECT_NOTE = 0xc130f0;`, `constexpr uint32_t VOICE_OWNER_OFFSET = 0x0a;`.
- **Shared values:** if the interpreter and native paths use the same value, define it once in a shared header.
- **Example:** in the proposal, show at least one sample declaration that uses the name.