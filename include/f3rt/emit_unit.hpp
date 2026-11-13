#pragma once
// Emit units: game code spans that draw one unit (object, queue record) into
// sprite RAM. Declared per game in config.toml [[video.emit_units]] and compiled
// into the generated sprite_units.hpp (tools/compile_sprite_units.py).
//
// They are behaviour anchors only: sprite grouping and origins come from the generated
// sprite_groups.hpp table (include/f3rt/sprite_group.hpp), so only `name`, `starts`,
// `ends`, `reg` and `size` are used at runtime.
#include <cstdint>
#include <span>

namespace f3rt {

// Register whose value at unit start addresses the unit's bytes in work RAM.
enum class UnitRegister : uint8_t { D0, D1, D2, D3, D4, D5, D6, D7, A0, A1, A2, A3, A4, A5, A6 };

struct EmitUnit {
    uint32_t id;                      // index into sprite_units::all, passed to f3_unit_enter/exit
    const char *name;                 // config name, a C identifier
    std::span<const uint32_t> starts; // span start PCs (hook before that instruction)
    std::span<const uint32_t> ends;   // ends[i] terminates starts[i]
    UnitRegister reg;
    uint32_t size; // bytes behaviours may read/patch at the unit address
};

// Inclusive PC range. Game video tables (games/*/video/video.cpp) use it; the
// generated sprite_units.hpp no longer declares frame-writer ranges.
struct PcRange {
    uint32_t first, last;
};

constexpr bool contains(PcRange range, uint32_t pc) {
    return pc >= range.first && pc <= range.last;
}

} // namespace f3rt
