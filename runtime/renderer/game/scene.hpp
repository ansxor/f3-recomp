#pragma once
#include "f3rt/game_video.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace f3rt {

// Hardware display-list limit: canonical (serialized) sprite lists never exceed it.
inline constexpr size_t MAX_HARDWARE_SPRITES = 1024;
// Render-only presented list (unit splices): full-detail splices expand spliced objects ~17x
// (measured peak 1300 presented sprites in Command War attract), so it needs its own capacity.
// Overflow is counted (GameVideo::report), never silent.
inline constexpr size_t MAX_PRESENTED_SPRITES = 4096;

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
    uint64_t group = 0;
    int32_t group_x = 0, group_y = 0;
    uint32_t group_pc = 0;
    float blend_weight = 1.0f;
    bool weighted() const { return blend_weight < 1.0f; }
    bool operator==(const SceneSprite &o) const {
        return x == o.x && y == o.y && scale_x == o.scale_x && scale_y == o.scale_y &&
               tile == o.tile && palette == o.palette && flip_x == o.flip_x && flip_y == o.flip_y;
    }
};

struct SceneLayer {
    uint8_t priority = 0;
    uint8_t blend_mode = 0;
    // Raw mix-word fields. A set c bit enables that window, and a set i bit selects inside.
    uint8_t clip_enabled = 0, clip_inverted = 0;
    bool clip_inverse = false;
    bool enabled = false;
    bool blend_select = false;
    bool mosaic = false;
    bool operator==(const SceneLayer &) const = default;
};

struct ScenePlayfield {
    SceneLayer layer;
    int32_t source_x = 0; // 24.8 source coordinate at native screen column zero.
    int32_t source_y = 0; // Integer source row before global screen flipping.
    int32_t x_step = 256;
    int32_t y_step = 256;
    uint8_t y_fraction = 0; // Native subpixel phase retained for high-resolution sampling.
    uint16_t palette_add = 0;
    // PF2/PF3 alternate physical map (maps 4/5) selected by line RAM bit 0x200;
    // only ever set when game_config::video.extended_alt_maps.
    bool alt_map = false;
    // The sampled map row has no nonzero tile code, so the hardware skips the layer on this
    // scanline (GameLines::cull_empty_rows). Derived each frame, never serialized.
    bool empty_row = false;
    // Render-only presentation hint, derived each frame (GameLines::resolve_alt_maps),
    // never serialized and never compared with the oracle.
    int16_t full_res_offset = -1;
};

struct SceneClip {
    int16_t left = 0, right = 0; // Half-open scanout coordinates, calibration applied.
    bool operator==(const SceneClip &) const = default;
};

// Scene layer numbering shared by the GPU scene encoding, sprite-plane bits, shaders and
// --video-layer-mask: 0..3 playfields, 4..7 sprites, 8 text. Values are fixed.
enum class LayerId : uint8_t { Pf0, Pf1, Pf2, Pf3, Sp0, Sp1, Sp2, Sp3, Text };
enum class LayerKind : uint8_t { Playfield, Sprite, Text };
inline constexpr unsigned LAYER_COUNT = 9;
inline constexpr uint32_t ALL_LAYERS = (1u << LAYER_COUNT) - 1;
static_assert(unsigned(LayerId::Text) == 8);

constexpr uint32_t layer_bit(LayerId id) { return 1u << unsigned(id); }
constexpr LayerKind kind(LayerId id) {
    return unsigned(id) < 4      ? LayerKind::Playfield
           : id == LayerId::Text ? LayerKind::Text
                                 : LayerKind::Sprite;
}
constexpr unsigned sub_index(LayerId id) {
    switch (kind(id)) {
    case LayerKind::Playfield:
        return unsigned(id);
    case LayerKind::Sprite:
        return unsigned(id) - unsigned(LayerId::Sp0);
    case LayerKind::Text:
        return 0;
    }
    return 0;
}
constexpr LayerId playfield(unsigned i) { return LayerId(i); }
constexpr LayerId sprite(unsigned i) { return LayerId(unsigned(LayerId::Sp0) + i); }
// An indexed sprite-plane value carries its sprite layer in bits 10-11.
constexpr LayerId sprite_layer(uint16_t plane_value) { return sprite((plane_value >> 10) & 3); }

struct LayerInfo {
    const char *name;
    uint16_t width, height;
    const char *domain; // sampling-domain suffix; reports print "<width>x<height>-<domain>"
};
inline constexpr unsigned PF_LAYER_WIDTH = game_config::video.extend ? 1024 : 512;
inline constexpr std::array<LayerInfo, LAYER_COUNT> LAYER_INFO{{
    {"pf0", PF_LAYER_WIDTH, 512, "indexed-texture"},
    {"pf1", PF_LAYER_WIDTH, 512, "indexed-texture"},
    {"pf2", PF_LAYER_WIDTH, 512, "indexed-texture"},
    {"pf3", PF_LAYER_WIDTH, 512, "indexed-texture"},
    {"sp0", geometry::NATIVE_WIDTH, geometry::HEIGHT, "next-sprite-plane"},
    {"sp1", geometry::NATIVE_WIDTH, geometry::HEIGHT, "next-sprite-plane"},
    {"sp2", geometry::NATIVE_WIDTH, geometry::HEIGHT, "next-sprite-plane"},
    {"sp3", geometry::NATIVE_WIDTH, geometry::HEIGHT, "next-sprite-plane"},
    {"text", 512, 512, "indexed-texture"},
}};
// Layers with scroll geometry (playfields and text).
inline constexpr std::array<LayerId, 5> SCROLLED_LAYERS{LayerId::Pf0, LayerId::Pf1, LayerId::Pf2,
                                                        LayerId::Pf3, LayerId::Text};

struct SceneRow {
    std::array<ScenePlayfield, 4> playfields{};
    std::array<SceneLayer, 4> sprites{};
    SceneLayer text;
    const SceneLayer &layer(LayerId id) const {
        switch (kind(id)) {
        case LayerKind::Playfield:
            return playfields[sub_index(id)].layer;
        case LayerKind::Sprite:
            return sprites[sub_index(id)];
        case LayerKind::Text:
            break;
        }
        return text;
    }
    SceneLayer &layer(LayerId id) {
        return const_cast<SceneLayer &>(std::as_const(*this).layer(id));
    }
    std::array<SceneClip, 4> clips{};
    std::array<uint8_t, 4> blend{}; // Saturated weights, 0..8.
    uint16_t background = 0;
    int16_t text_x = 0, text_y = 0;
    uint8_t mosaic_period = 16;
    bool bitmap = false;
    // Line RAM 0x6000 section word 2 bits 14/13 (MAME y-ack/fdp-collapse): 15-bit palette
    // words and a horizontal two-pixel blur. Not serialized: rows are re-decoded every VBSTART.
    bool palette_15bit = false, blur = false;
};

// Presented pixel byte order of the scale-1 indexed compositor.
enum class PixelFormat { Argb8888, Rgba8888 };
constexpr uint32_t to_pixel_format(uint32_t argb, PixelFormat format) {
    return format == PixelFormat::Rgba8888 ? (argb << 8) | 0xff : argb;
}
// Which sprite list the compositor/encoder draws.
enum class SpriteList { Presented, Canonical };

struct ClipRanges {
    // Four inside intervals leave at most five gaps. With an outside window,
    // at most three inside intervals can join the two edge intervals.
    static constexpr unsigned capacity = 5;
    std::array<SceneClip, capacity> ranges{};
    unsigned count = 0;

    const SceneClip *begin() const { return ranges.data(); }
    const SceneClip *end() const { return ranges.data() + count; }
};

inline ClipRanges clip_ranges(const std::array<SceneClip, 4> &windows, unsigned enabled,
                              unsigned inside, bool invert, int16_t left, int16_t right) {
    if (left >= right) return {};

    ClipRanges parts;
    if (!enabled) {
        if (invert) {
            parts.ranges[0] = {left, right};
            parts.count = 1;
        }
        return parts;
    }
    // The union of all selected outsides is the complement of their common inside.
    SceneClip common_inside{left, right};
    for (unsigned n = 0; n < windows.size(); ++n) {
        if (!(enabled & (1u << n))) continue;
        const SceneClip window{std::max(left, windows[n].left), std::min(right, windows[n].right)};
        if (inside & (1u << n)) {
            if (window.left < window.right) parts.ranges[parts.count++] = window;
        } else {
            common_inside.left = std::max(common_inside.left, window.left);
            common_inside.right = std::min(common_inside.right, window.right);
        }
    }
    if (common_inside.left >= common_inside.right) {
        if (invert) return {};
        parts.ranges[0] = {left, right};
        parts.count = 1;
        return parts;
    }
    if (left < common_inside.left) parts.ranges[parts.count++] = {left, common_inside.left};
    if (common_inside.right < right) parts.ranges[parts.count++] = {common_inside.right, right};

    for (unsigned i = 1; i < parts.count; ++i) {
        const auto part = parts.ranges[i];
        unsigned j = i;
        while (j && parts.ranges[j - 1].left > part.left) {
            parts.ranges[j] = parts.ranges[j - 1];
            --j;
        }
        parts.ranges[j] = part;
    }
    ClipRanges visible;
    for (const auto part : parts) {
        if (visible.count && part.left <= visible.ranges[visible.count - 1].right) {
            auto &last = visible.ranges[visible.count - 1];
            last.right = std::max(last.right, part.right);
        } else {
            visible.ranges[visible.count++] = part;
        }
    }
    if (!invert) return visible;

    ClipRanges gaps;
    for (const auto part : visible) {
        if (left < part.left) gaps.ranges[gaps.count++] = {left, part.left};
        left = part.right;
    }
    if (left < right) gaps.ranges[gaps.count++] = {left, right};
    return gaps;
}

inline ClipRanges clip_ranges(const SceneRow &row, const SceneLayer &layer, int16_t left,
                              int16_t right) {
    return clip_ranges(row.clips, layer.clip_enabled, layer.clip_inverted, layer.clip_inverse,
                       left, right);
}

} // namespace f3rt
