#pragma once
#include "f3rt_video_cxx/video.h"
#include <cstdint>
#include <span>

namespace f3rt {

// Decoded 16x16 6bpp sprite and playfield tiles (Rust `GraphicsAssets`).
class GraphicsAssets {
public:
    GraphicsAssets();
    ~GraphicsAssets();
    GraphicsAssets(const GraphicsAssets &) = delete;
    GraphicsAssets &operator=(const GraphicsAssets &) = delete;
    GraphicsAssets(GraphicsAssets &&) noexcept;
    GraphicsAssets &operator=(GraphicsAssets &&) noexcept;

    bool load_roms(std::span<const uint8_t> sprites, std::span<const uint8_t> sprites_hi,
                   std::span<const uint8_t> tilemap, std::span<const uint8_t> tilemap_hi);
    std::span<const uint8_t> sprite_tiles() const;
    std::span<const uint8_t> playfield_tiles() const;
    const uint8_t *sprite_tile(uint32_t code) const;
    const uint8_t *playfield_tile(uint32_t code) const;

    const rust_video::GraphicsAssets &inner() const { return *m_assets; }

private:
    rust::Box<rust_video::GraphicsAssets> m_assets;
};

} // namespace f3rt
