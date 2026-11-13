#pragma once
#include <cstdint>
#include <cstddef>
#include <span>

namespace f3rt {

inline constexpr uint16_t SPRITE_ENTRIES = 0x400;
inline constexpr size_t SPRITE_SLOTS = 0x800;

constexpr uint16_t sprite_slot_for(uint32_t offset) {
    return uint16_t(((offset >> 15) & 1) * SPRITE_ENTRIES + ((offset & 0x7fff) >> 4));
}
constexpr bool sprite_slot_listed(uint32_t offset) {
    return ((offset & 0x7fff) >> 4) < SPRITE_ENTRIES;
}

struct SpriteGroupSlot {
    uint64_t key = 0;
    int32_t x = 0, y = 0;
    uint32_t pc = 0;
    bool operator==(const SpriteGroupSlot &) const = default;
};

struct SpriteSplice {
    bool bank = false;                    // false: 0x600000 half, true: 0x608000 half
    uint16_t first = 0, last = 0;         // inclusive entry indices within the bank (0..0x3ff)
    std::span<const uint8_t> real;
    std::span<const uint8_t> replacement; // n * 16 raw entries; jump words inside are ignored
    uint64_t group = 0;
    int32_t group_x = 0, group_y = 0;
    uint32_t group_pc = 0;
};

struct SpriteRamWrite {
    uint32_t start = 0;
    unsigned width = 0;
};

class SpriteRamWriteListener {
public:
    virtual ~SpriteRamWriteListener() = default;
    virtual void sprite_group_write(const SpriteRamWrite &write) = 0;
};

struct SpritePresentation {
    std::span<const SpriteGroupSlot> groups;
    std::span<const SpriteSplice> splices;
    bool empty() const { return groups.empty() && splices.empty(); }
};

}
