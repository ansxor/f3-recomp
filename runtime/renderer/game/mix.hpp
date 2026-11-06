#pragma once
#include "renderer/game/scene.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {
struct PixelMix {
    uint16_t source, destination;
    uint8_t source_weight, destination_weight;
    uint8_t source_priority, destination_priority, source_mode;
};

inline void mix(PixelMix &pixel, const SceneLayer &layer, uint16_t color, bool select,
                const std::array<uint8_t, 4> &blend) {
    if (layer.blend_mode == pixel.source_mode) return;
    if (layer.priority > pixel.source_priority) {
        unsigned slot = unsigned(select);
        if (layer.blend_mode == 1 || layer.blend_mode == 2) {
            if (layer.blend_mode == 1) slot += 2;
            if (!blend[slot]) return;
            pixel.source_weight = blend[slot];
        } else {
            if (!blend[slot] && !blend[slot + 2]) return;
            pixel.source_weight = blend[slot + 2];
            pixel.destination_weight = blend[slot];
            pixel.destination_priority = layer.priority;
            pixel.destination = color;
        }
        pixel.source = color;
        pixel.source_mode = layer.blend_mode;
        pixel.source_priority = layer.priority;
    } else if (layer.priority >= pixel.destination_priority) {
        pixel.destination = layer.priority == pixel.destination_priority ? 0 : color;
        pixel.destination_priority = layer.priority;
        pixel.destination_weight = blend[unsigned(select) + (pixel.source_mode == 1 ? 0 : 2)];
    }
}

// FDA 15-bit palette word RRRRGGGGBBBBRGBx: nibbles shifted, low bits fill the lsb (not replicated).
inline uint32_t color15(uint32_t color) {
    return ((color & 0xf000u) << 8) | ((color & 8u) << 16) | ((color & 0x0f00u) << 4) |
           ((color & 4u) << 9) | (color & 0x00f0u) | ((color & 2u) << 2);
}

template <bool Palette15> uint32_t rgb(const PixelMix &pixel, std::span<const uint32_t> palette) {
    auto fetch = [&](uint16_t index) {
        const uint32_t color = palette[index & 8191];
        return Palette15 ? color15(color) : color;
    };
    if (pixel.source_weight == 8 && pixel.destination_weight == 0)
        return 0xff000000u | fetch(pixel.source);
    if (pixel.source_weight == 0 && pixel.destination_weight == 8)
        return 0xff000000u | fetch(pixel.destination);
    const uint32_t source = fetch(pixel.source);
    const uint32_t destination = fetch(pixel.destination);
    uint32_t color = 0xff000000;
    for (unsigned shift : {0u, 8u, 16u}) {
        const unsigned value = (((source >> shift) & 255) * pixel.source_weight +
                                ((destination >> shift) & 255) * pixel.destination_weight) >>
                               3;
        color |= std::min(value, 255u) << shift;
    }
    return color;
}

constexpr PixelMix background_mix(uint16_t background) { return {0, background, 0, 8, 0, 0, 255}; }
} // namespace f3rt
