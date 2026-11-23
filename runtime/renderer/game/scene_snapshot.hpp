#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt/renderer/graphics_assets.hpp"
#include "f3rt_video_cxx/video.h"
#include "renderer/game/scene.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {
struct VideoConfig;

// Host-only, CPU-typed snapshot of one decoded scanout frame; never serialized into machine
// state. The authoritative copy is a Rust `rust_video::SceneSnapshot`; the fields below are a
// mirror for host code and tests, refreshed from Rust by `refresh()` and pushed back by
// `flush()` whenever the mirror was edited (only live element counts move).
//
// GameVideo overwrites its snapshot at VBSTART after the layer components decode video RAM.
// Everything downstream encodes it into the GPU word layout once per present.
struct SceneSnapshot {
    static constexpr size_t MAX_SPRITES = MAX_PRESENTED_SPRITES, PALETTE_SIZE = 8192;
    std::array<SceneRow, 256> rows{};
    std::array<SceneSprite, MAX_SPRITES> sprites{};
    unsigned sprite_count = 0;
    std::array<SceneSprite, MAX_HARDWARE_SPRITES> canonical_sprites{};
    unsigned canonical_sprite_count = 0;
    std::array<uint32_t, PALETTE_SIZE> colors{}; // 0x00RRGGBB, indexed by palette entry
    uint8_t pen_mask = 15;
    bool flipped = false;
    bool sprite_flipped = false;
    std::vector<uint16_t> sprite_trail_seed;
    // Oracle-fallback frame: only native_pixels (320 x geometry::HEIGHT ARGB) is meaningful.
    bool fallback = true;
    std::array<uint32_t, geometry::NATIVE_PIXELS> native_pixels{};

    SceneSnapshot();
    explicit SceneSnapshot(const VideoConfig &config);
    SceneSnapshot(const SceneSnapshot &other);
    SceneSnapshot &operator=(const SceneSnapshot &other);
    ~SceneSnapshot();

    // Everything below refreshes the mirror from Rust first, so direct field edits made since the
    // last refresh are not lost (the mirror is authoritative only between refresh and flush).
    std::span<const SceneSprite> sprite_list() const;
    std::span<const SceneSprite> sprite_list(SpriteList list) const;
    std::span<const SceneSprite> canonical_sprite_list() const;
    bool presents_enhanced_sprites() const;
    unsigned sprite_count_for(SpriteList list) const;
    bool fallback_frame() const;
    unsigned trail_len() const;
    std::vector<uint16_t> text_map() const;
    void set_text_map(std::span<const uint16_t> map);
    ScenePixel text_pixel(int x, int y, bool flipped) const;

    // Make this an oracle-fallback frame: `native` (320 x geometry::HEIGHT ARGB) becomes
    // native_pixels, or opaque black when empty.
    SceneSnapshot &capture_fallback(std::span<const uint32_t> native = {});
    // Scale-1 CPU indexed composition with optional symmetric horizontal `border` columns.
    void compose(std::span<uint32_t> output, size_t stride, PixelFormat format, unsigned border,
                 const GraphicsAssets &assets, SpriteList list = SpriteList::Presented) const;

    // Rust snapshot for the bridge. `inner()` pushes the mirror first; `destination()` is a
    // capture target and keeps the Rust state untouched.
    const rust_video::SceneSnapshot &inner() const;
    rust_video::SceneSnapshot &destination() const { return *m_inner; }

    void refresh() const; // Rust -> mirror
    void flush() const;   // mirror -> Rust

private:
    mutable rust::Box<rust_video::SceneSnapshot> m_inner;
    mutable bool m_mirror_dirty = false;
};

} // namespace f3rt
