#pragma once
#include "f3rt_sprites_cxx/sprites.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace f3rt {

inline constexpr size_t SPRITE_SLOTS = 0x800;

constexpr uint64_t pack_group_key(uint32_t base, unsigned group_class, uint16_t gen_x,
                                   uint16_t gen_y) {
    return (uint64_t(base & 0xffffff) << 40) | (uint64_t(group_class & 0xff) << 32) |
           (uint64_t(gen_x) << 16) | uint64_t(gen_y);
}

using SpriteGroupSlot = rust_sprites::SpriteGroupSlot;

struct SpritePresentation {
    std::span<const SpriteGroupSlot> groups;
    bool empty() const { return groups.empty(); }
};

}
