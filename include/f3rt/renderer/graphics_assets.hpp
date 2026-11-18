#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {

class GraphicsAssets {
public:
    bool load_roms(std::span<const uint8_t> sprites, std::span<const uint8_t> sprites_hi,
                   std::span<const uint8_t> tilemap, std::span<const uint8_t> tilemap_hi);
    std::span<const uint8_t> sprite_tiles() const { return m_decoded_sprites; }
    std::span<const uint8_t> playfield_tiles() const { return m_decoded_tiles; }
    const uint8_t *sprite_tile(uint32_t code) const;
    const uint8_t *playfield_tile(uint32_t code) const;

private:
    static const uint8_t *tile(std::span<const uint8_t> pixels, uint32_t code, uint32_t count,
                               uint32_t mask);
    uint32_t m_sprite_tile_mask = 0, m_playfield_tile_mask = 0;
    uint32_t m_sprite_tile_count = 0, m_playfield_tile_count = 0;
    std::vector<uint8_t> m_decoded_sprites;
    std::vector<uint8_t> m_decoded_tiles;
};

} // namespace f3rt
