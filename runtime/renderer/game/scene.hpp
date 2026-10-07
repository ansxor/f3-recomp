#pragma once
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

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

class StateWriter;
class StateReader;

// A scene component decoded from video RAM at VBSTART.
template<class T>
concept SceneSource = requires(T &t, const VideoRam &vram) {
    t.reset();
    t.decode(vram);
};

// A component serialized into machine/netplay snapshots; components rebuilt
// purely from video RAM are not.
template<class T>
concept Snapshotable = requires(const T &c, T &t, StateWriter &w, StateReader &r) {
    { c.state_size() } -> std::same_as<size_t>;
    c.save_state(w);
    t.load_state(r);
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
    bool operator==(const SceneSprite &) const = default;
};

struct SceneLayer {
    uint8_t priority = 0;
    uint8_t blend_mode = 0;
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
};

struct SceneClip {
    int16_t left = 0, right = 0; // Half-open scanout coordinates, calibration applied.
    bool operator==(const SceneClip &) const = default;
};

// Scene layer numbering shared by the GPU scene encoding, sprite-plane bits, shaders and
// --video-layer-mask: 0..3 playfields, 4..7 sprites, 8 text. Values are fixed.
enum class LayerId : uint8_t { Pf0, Pf1, Pf2, Pf3, Sp0, Sp1, Sp2, Sp3, Text };
enum class LayerKind : uint8_t { Playfield, Sprite, Text };
inline constexpr unsigned layer_count = 9;
inline constexpr uint32_t all_layers = (1u << layer_count) - 1;
static_assert(unsigned(LayerId::Text) == 8);

constexpr uint32_t layer_bit(LayerId id) { return 1u << unsigned(id); }
constexpr LayerKind kind(LayerId id) {
    return unsigned(id) < 4 ? LayerKind::Playfield : id == LayerId::Text ? LayerKind::Text : LayerKind::Sprite;
}
constexpr unsigned sub_index(LayerId id) {
    switch (kind(id)) {
    case LayerKind::Playfield: return unsigned(id);
    case LayerKind::Sprite: return unsigned(id) - unsigned(LayerId::Sp0);
    case LayerKind::Text: return 0;
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
    const char *domain;
};
inline constexpr std::array<LayerInfo, layer_count> layer_info{{
    {"pf0", 1024, 512, "1024x512-indexed-texture"},
    {"pf1", 1024, 512, "1024x512-indexed-texture"},
    {"pf2", 1024, 512, "1024x512-indexed-texture"},
    {"pf3", 1024, 512, "1024x512-indexed-texture"},
    {"sp0", 320, 232, "320x232-next-sprite-plane"},
    {"sp1", 320, 232, "320x232-next-sprite-plane"},
    {"sp2", 320, 232, "320x232-next-sprite-plane"},
    {"sp3", 320, 232, "320x232-next-sprite-plane"},
    {"text", 512, 512, "512x512-indexed-texture"},
}};
// Layers with scroll geometry (playfields and text).
inline constexpr std::array<LayerId, 5> scrolled_layers{
    LayerId::Pf0, LayerId::Pf1, LayerId::Pf2, LayerId::Pf3, LayerId::Text};

struct SceneRow {
    std::array<ScenePlayfield, 4> playfields{};
    std::array<SceneLayer, 4> sprites{};
    SceneLayer text;
    const SceneLayer &layer(LayerId id) const {
        switch (kind(id)) {
        case LayerKind::Playfield: return playfields[sub_index(id)].layer;
        case LayerKind::Sprite: return sprites[sub_index(id)];
        case LayerKind::Text: break;
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
};

} // namespace f3rt
