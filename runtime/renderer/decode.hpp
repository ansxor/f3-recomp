// license:BSD-3-Clause
// Generic TC0630FDP video-RAM decode primitives, shared by the FDP renderer
// (runtime/renderer/fdp/video.cpp) and the per-game scene decoders under games/<game>/video/,
// so both read video RAM through one implementation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "renderer/sprite_presentation.hpp"

namespace f3rt {

// One 8x8 4bpp character tile: 32 raw bytes -> 64 packed-nibble pixels
// (row[3 - x/2] >> ((x & 1) * 4)).
inline void decode_charram_tile(const uint8_t *tile_src, uint8_t *dest) {
    for (int y = 0; y < 8; ++y) {
        const uint8_t *row = &tile_src[y * 4];
        for (int x = 0; x < 8; ++x)
            dest[y * 8 + x] = (row[3 - x / 2] >> ((x & 1) * 4)) & 0x0f;
    }
}

// Hardware sprite display-list state that persists across frames. `bank` selects
// the active 0x8000 half of sprite RAM; the command word latched from word 5 of
// a list entry updates the rest.
struct SpriteRamState {
    bool flipscreen = false;
    bool bank = false;
    bool trails = false;
    uint8_t extra_planes = 0;
    uint8_t pen_mask = 0x0f;
};

// One decoded sprite-list entry. `x`/`y` are 24.8 scanout coordinates, unmirrored
// when apply_flip is false (the caller's sprite latch applies the flipscreen
// mirror) or already mirrored when true. `color` is the raw colour byte: its low
// 6 bits select the 16x16 tile palette and bits 6..7 its priority group.
struct DecodedSpriteEntry {
    int32_t x = 0, y = 0;
    int32_t scale_x = 256, scale_y = 256;
    uint32_t tile = 0;
    uint8_t color = 0;
    bool flip_x = false, flip_y = false;
    uint8_t pri = 0;
    uint64_t identity = 0; // stable sprite identity from a SpritePresentation; 0 = unknown
    uint64_t object = 0;   // owning invocation identity (shared by a multi-part object); 0 = unknown
};

// Walk the sprite display list in `spriteram` (0x10000 bytes, base 0x600000)
// following block chaining, the jump word and command entries (which update `state`).
// `visible_y`/`visible_height` are the FDP scanout crop used for culling.
// Returns the number of entries written to `out`.
size_t decode_sprite_list(const uint8_t *spriteram, int visible_y, int visible_height,
                          std::span<DecodedSpriteEntry> out, SpriteRamState &state,
                          bool apply_flip, const SpritePresentation *presentation = nullptr);

} // namespace f3rt
