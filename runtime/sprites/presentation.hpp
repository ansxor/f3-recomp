#pragma once
// Render-only view of sprite RAM produced by the sprite system and consumed by both sprite
// decoders (FDP and GameSprites). Nothing here is serialized; enabling it must never change
// Machine::state_crc().
#include <cstdint>
#include <cstddef>
#include <span>

namespace f3rt {

// Sprite-RAM geometry. Offsets are relative to GRAPHICS_BASE: two 0x400-entry banks, the low
// half at 0x0000..0x7fff and the high half at 0x8000..0xffff (entry 0x10 bytes). Slots are
// indexed bank * SPRITE_ENTRIES + entry.
inline constexpr uint16_t SPRITE_ENTRIES = 0x400;
inline constexpr size_t SPRITE_SLOTS = 0x800;

constexpr uint16_t sprite_slot_for(uint32_t offset) {
    return uint16_t(((offset >> 15) & 1) * SPRITE_ENTRIES + ((offset & 0x7fff) >> 4));
}
// False for the bytes of the second half of each 0x8000 bank: hardware never displays them.
constexpr bool sprite_slot_listed(uint32_t offset) {
    return ((offset & 0x7fff) >> 4) < SPRITE_ENTRIES;
}

// Sprite group of one slot: the stable key (0 = unresolved) and the group origin at the time
// the slot was written (24.8 scanout units). Params of a rigid group: member position =
// origin + lane offset.
struct SpriteGroupSlot {
    uint64_t key = 0;
    int32_t x = 0, y = 0;
    uint32_t pc = 0; // sprite-RAM writer PC that set the slot's group (render-only)
    bool operator==(const SpriteGroupSlot &) const = default;
};

// Replace bank entries [first, last] with `replacement` while decoding. A splice applies only
// while the bank still holds exactly the `real` bytes (content validation makes stale behaviour
// splices harmless across bank flips, lag and state loads).
struct SpriteSplice {
    bool bank = false;                    // false: 0x600000 half, true: 0x608000 half
    uint16_t first = 0, last = 0;         // inclusive entry indices within the bank (0..0x3ff)
    std::span<const uint8_t> real;        // validated range
    std::span<const uint8_t> replacement; // n * 16 raw entries; jump words inside are ignored
    // Group of the replacement entries (0 = unknown) and its origin at splice time.
    uint64_t group = 0;
    int32_t group_x = 0, group_y = 0;
    uint32_t group_pc = 0;
};

// One sprite-RAM store, reported to the write listeners instead of them reaching into the
// tracker's write state.
struct SpriteRamWrite {
    uint32_t start = 0; // byte offset within sprite RAM (relative to GRAPHICS_BASE)
    unsigned width = 0;
};

// Render-only listener for every sprite-RAM write.
class SpriteRamWriteListener {
public:
    virtual ~SpriteRamWriteListener() = default;
    virtual void sprite_group_write(const SpriteRamWrite &write) = 0;
};

struct SpritePresentation {
    // 0x800 entries indexed bank * 0x400 + entry. Empty span = no groups.
    std::span<const SpriteGroupSlot> groups;
    // Sorted by (bank, first); non-overlapping. `real`-validated behaviour splices.
    std::span<const SpriteSplice> splices;
    bool empty() const { return groups.empty() && splices.empty(); }
};

} // namespace f3rt
