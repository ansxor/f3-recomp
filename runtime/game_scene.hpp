#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Display producers may read game code/assets and work RAM, never FDP RAM.
// An unsupported source invalidates its component; callers must use the oracle.
struct GameMemory {
    std::span<const uint8_t> rom;
    std::span<const uint8_t> ram;
    bool supported = true;

    uint8_t u8(uint32_t address) {
        address &= 0xffffff;
        if (address < rom.size()) return rom[address];
        if (address >= 0x400000 && address < 0x440000 && ram.size() == 0x20000)
            return ram[address & 0x1ffff];
        supported = false;
        return 0;
    }
    uint16_t u16(uint32_t address) {
        const uint16_t high = u8(address);
        return uint16_t((high << 8) | u8(address + 1));
    }
    uint32_t u32(uint32_t address) {
        const uint32_t high = u16(address);
        return (high << 16) | u16(address + 2);
    }
};

struct ScenePixel {
    uint16_t palette = 0;
    uint8_t flags = 0; // bit 4: nontransparent texel; bit 0: blend selector.
};

struct SceneSprite {
    int32_t x = 0, y = 0; // 24.8 coordinates in the 432x256 scanout space.
    uint16_t scale_x = 256, scale_y = 256;
    uint32_t tile = 0;
    uint8_t palette = 0;
    bool flip_x = false, flip_y = false;
};

struct SceneLayer {
    uint8_t priority = 0;
    uint8_t blend_mode = 0;
    uint8_t clip_enabled = 0, clip_inverted = 0;
    bool clip_inverse = false;
    bool enabled = false;
    bool blend_select = false;
    bool mosaic = false;
};

struct ScenePlayfield {
    SceneLayer layer;
    int32_t source_x = 0; // 24.8 source coordinate at native screen column zero.
    int32_t source_y = 0; // Integer source row before global screen flipping.
    int32_t x_step = 256;
    uint16_t palette_add = 0;
};

struct SceneClip {
    int16_t left = 0, right = 0; // Half-open scanout coordinates, calibration applied.
};

struct SceneRow {
    std::array<ScenePlayfield, 4> playfields{};
    std::array<SceneLayer, 4> sprites{};
    SceneLayer text;
    std::array<SceneClip, 4> clips{};
    std::array<uint8_t, 4> blend{}; // Saturated weights, 0..8.
    uint16_t background = 0;
    int16_t text_x = 0, text_y = 0;
    uint8_t mosaic_period = 16;
    bool bitmap = false;
};

} // namespace f3rt
