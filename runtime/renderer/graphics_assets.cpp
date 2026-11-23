// license:BSD-3-Clause
#include "f3rt/renderer/graphics_assets.hpp"

namespace f3rt {
namespace {
template <class T> std::span<const uint8_t> bytes_of(const rust::Slice<const T> &slice) {
    return {reinterpret_cast<const uint8_t *>(slice.data()), slice.size()};
}
} // namespace

GraphicsAssets::GraphicsAssets() : m_assets(rust_video::new_graphics_assets()) {}
GraphicsAssets::~GraphicsAssets() = default;
GraphicsAssets::GraphicsAssets(GraphicsAssets &&) noexcept = default;
GraphicsAssets &GraphicsAssets::operator=(GraphicsAssets &&) noexcept = default;

bool GraphicsAssets::load_roms(std::span<const uint8_t> sprites, std::span<const uint8_t> sprites_hi,
                               std::span<const uint8_t> tilemap, std::span<const uint8_t> tilemap_hi) {
    const auto view = [](std::span<const uint8_t> span) {
        return rust::Slice<const uint8_t>(span.data(), span.size());
    };
    return m_assets->load_roms(view(sprites), view(sprites_hi), view(tilemap), view(tilemap_hi));
}

std::span<const uint8_t> GraphicsAssets::sprite_tiles() const {
    return bytes_of(m_assets->sprite_tiles());
}

std::span<const uint8_t> GraphicsAssets::playfield_tiles() const {
    return bytes_of(m_assets->playfield_tiles());
}

const uint8_t *GraphicsAssets::sprite_tile(uint32_t code) const {
    return m_assets->sprite_tile(code).data();
}

const uint8_t *GraphicsAssets::playfield_tile(uint32_t code) const {
    return m_assets->playfield_tile(code).data();
}

} // namespace f3rt
