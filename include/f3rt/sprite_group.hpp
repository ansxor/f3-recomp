#pragma once
#include <cstdint>
#include <span>
namespace f3rt {
enum class SpriteLane : uint8_t { None = 0, X = 1, Y = 2, XY = 3 };
// Base register encoding matches f3rt::UnitRegister (0..7 = d0..d7, 8..14 = a0..a6).
struct SpriteGroupWrite {
    uint32_t pc = 0;         // PC of the instruction that stores the X and/or Y sprite word(s)
    uint8_t base = 0;        // register holding the object/queue record at that store
    uint8_t lane = 0;        // SpriteLane bits
    uint8_t group_class = 0; // distinct root-field pair id (1..255)
    uint8_t shift = 8;       // root delta -> 24.8 scanout units
    uint16_t x_offset = 0;   // root X field offset from base (0xffff when lane has no X)
    uint16_t y_offset = 0;   // root Y field offset from base (0xffff when lane has no Y)
};
struct SpriteGroupReset {
    uint32_t pc = 0;
    uint8_t base = 0;
    uint8_t pad = 0;
    uint16_t offset = 0;     // root field offset from base
};
struct SpriteGroupTable {
    std::span<const SpriteGroupWrite> writes;
    std::span<const SpriteGroupReset> resets;
};
}
