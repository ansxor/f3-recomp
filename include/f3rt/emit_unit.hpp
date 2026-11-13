#pragma once
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

struct PcRange {
    uint32_t first, last;
};

constexpr bool contains(PcRange range, uint32_t pc) {
    return pc >= range.first && pc <= range.last;
}

} // namespace f3rt
