#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Video RAM as seen at VBSTART. Offsets match Machine::graphics (0x600000) and
// Machine::control (0x660000); data is big-endian as stored by the 68020.
struct VideoRam {
    std::span<const uint8_t> graphics; // 0x40000 bytes
    std::span<const uint8_t> control;  // 0x20 bytes
    uint64_t frame = 0;                // Machine::frame + 1; diagnostics only
    uint16_t u16(uint32_t offset) const {
        return uint16_t(uint16_t(graphics[offset]) << 8 | graphics[offset + 1]);
    }
    uint16_t control_u16(unsigned index) const {
        return uint16_t(uint16_t(control[index * 2]) << 8 | control[index * 2 + 1]);
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
    int32_t y_step = 256;
    uint8_t y_fraction = 0; // Native subpixel phase retained for high-resolution sampling.
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
