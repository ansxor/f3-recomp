#pragma once
#include "f3rt/game_video.hpp"
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

// A component serialized into machine snapshots; components rebuilt
// purely from video RAM are not.
template<class T>
concept Snapshotable = requires(const T &c, T &t, StateWriter &w, StateReader &r) {
    { c.state_size() } -> std::same_as<size_t>;
    c.save_state(w);
    t.load_state(r);
};

// Tile index within an asset ROM of `count` 16x16 tiles (the ROM decides the
// wrap, not a fixed 15-bit field): power-of-two ROMs mask, others take the remainder.
// Mirrors the FDP's wrap_tile; GLSL `wrap_tile` in scene.glsl is the GPU twin.
constexpr uint32_t wrap_tile_index(uint32_t code, uint32_t count) {
    return (count & (count - 1)) == 0 ? code & (count - 1) : code % count;
}

// Hardware display-list limit: canonical (serialized) sprite lists never exceed it.
inline constexpr size_t max_hardware_sprites = 1024;
// Render-only presented list (unit splices): full-detail splices expand spliced objects ~17x
// (measured peak 1300 presented sprites in Command War attract), so it needs its own capacity.
// Overflow is counted (GameVideo::report), never silent.
inline constexpr size_t max_presented_sprites = 4096;

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
    // Render-only stable object identity (0 = unknown); never serialized and not part of
    // geometry equality, so identity-only changes are not motion.
    uint64_t identity = 0;
    // Owning invocation identity shared by all parts of one object (0 = unknown); render-only and
    // excluded from equality like `identity`. Motion uses it for the per-object rigid check.
    uint64_t object = 0;
    // Render-only flicker shadow tag (sprite_flag_shadow: written by the game's shadow emit path); the
    // GPU presenter picks its visibility per presented frame. Excluded from equality like `identity`.
    bool shadow = false;
    bool operator==(const SceneSprite &o) const {
        return x == o.x && y == o.y && scale_x == o.scale_x && scale_y == o.scale_y && tile == o.tile &&
               palette == o.palette && flip_x == o.flip_x && flip_y == o.flip_y;
    }
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
    // PF2/PF3 alternate physical map (maps 4/5) selected by line RAM bit 0x200;
    // only ever set when game_config::video.extended_alt_maps.
    bool alt_map = false;
    // The sampled map row has no nonzero tile code, so the hardware skips the layer on this
    // scanline (GameLines::cull_empty_rows). Derived each frame, never serialized.
    bool empty_row = false;
    // Render-only presentation hint, derived each frame (GameLines::resolve_full_resolution_alt),
    // never serialized and never compared with the oracle: for an alt_map row, the offset c in
    // [0, pf_map_width_px) with alt(x, y) == main((2x + c) mod pf_map_width_px, y) for every tile
    // row the scanline samples; -1 = none (the canonical alternate-map sampling stands).
    // Consumed only through presented_playfield().
    int16_t full_res_offset = -1;
};

// Playfield map width in texels (64 columns of 16).
inline constexpr unsigned pf_map_width_px = 1024;

// The presented (expanded GPU/CPU output) view of one playfield row. With
// game_config::video.full_resolution_alt_maps, an alt_map row that has a solved full-resolution
// twin samples the main map at twice the step: x' = 2x + c, so alt_map=false, x_step*2 and
// source_x = 2*source_x + (c << 8) wrapped to the map period (24.8). Vertical geometry is unchanged.
// Canonical rows, native composition and snapshots never call this.
constexpr ScenePlayfield presented_playfield(const ScenePlayfield &p) {
    if constexpr (game_config::video.full_resolution_alt_maps) {
        if (p.alt_map && p.full_res_offset >= 0) {
            ScenePlayfield q = p;
            q.alt_map = false;
            q.x_step = p.x_step * 2;
            q.source_x = int32_t((uint32_t(p.source_x) * 2u + (uint32_t(p.full_res_offset) << 8)) &
                                 (pf_map_width_px * 256u - 1u));
            return q;
        }
    }
    return p;
}

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
    const char *domain; // sampling-domain suffix; reports print "<width>x<height>-<domain>"
};
inline constexpr std::array<LayerInfo, layer_count> layer_info{{
    {"pf0", 1024, 512, "indexed-texture"},
    {"pf1", 1024, 512, "indexed-texture"},
    {"pf2", 1024, 512, "indexed-texture"},
    {"pf3", 1024, 512, "indexed-texture"},
    {"sp0", geometry::native_width, geometry::height, "next-sprite-plane"},
    {"sp1", geometry::native_width, geometry::height, "next-sprite-plane"},
    {"sp2", geometry::native_width, geometry::height, "next-sprite-plane"},
    {"sp3", geometry::native_width, geometry::height, "next-sprite-plane"},
    {"text", 512, 512, "indexed-texture"},
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
    // Line RAM 0x6000 section word 2 bits 14/13 (MAME y-ack/fdp-collapse): 15-bit palette
    // words and a horizontal two-pixel blur. Not serialized: rows are re-decoded every VBSTART.
    bool palette_15bit = false, blur = false;
};

} // namespace f3rt
