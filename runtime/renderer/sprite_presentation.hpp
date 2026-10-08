#pragma once
// Render-only view of sprite RAM produced by SpriteUnits and consumed by both
// sprite decoders (FDP and GameSprites). Nothing here is serialized; enabling it
// must never change Machine::state_crc()/sync_state_crc().
#include <cstdint>
#include <span>

namespace f3rt {

// Replace bank entries [first, last] with `replacement` while decoding, if and only
// if the bank still holds exactly `real` there (content validation makes stale
// splices harmless across bank flips, lag and state loads).
struct SpriteSplice {
    bool bank = false;                    // false: 0x600000 half, true: 0x608000 half
    uint16_t first = 0, last = 0;         // inclusive entry indices within the bank (0..0x3ff)
    std::span<const uint8_t> real;        // (last - first + 1) * 16 bytes captured at unit exit
    std::span<const uint8_t> replacement; // n * 16 raw entries; jump words inside are ignored
    uint64_t identity = 0;                // invocation identity; entry k gets sprite_identity_mix(identity, k)
};

struct SpritePresentation {
    // 0x800 entries indexed bank * 0x400 + entry; 0 = unknown. Empty span = no identities.
    std::span<const uint64_t> identity;
    // Sorted by (bank, first); non-overlapping.
    std::span<const SpriteSplice> splices;
    // Parallel to `identity`: the owning invocation's identity per entry (0 = unknown); empty = none.
    // Splice replacement entries take SpriteSplice::identity.
    std::span<const uint64_t> object;
    bool empty() const { return identity.empty() && splices.empty() && object.empty(); }
};

// Stable per-entry identity derived from an invocation identity; never returns 0.
constexpr uint64_t sprite_identity_mix(uint64_t identity, uint32_t index) {
    uint64_t x = identity ^ (uint64_t(index) + 0x9e3779b97f4a7c15ull);
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x ? x : 1;
}

} // namespace f3rt
