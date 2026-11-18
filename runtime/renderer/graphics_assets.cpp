// license:BSD-3-Clause
// copyright-holders:Bryan McPhail, ywy, 12Me21, f3rt authors
#include "f3rt/renderer/graphics_assets.hpp"

namespace f3rt {

bool GraphicsAssets::load_roms(std::span<const uint8_t> sprites, std::span<const uint8_t> sprites_hi,
                               std::span<const uint8_t> tilemap, std::span<const uint8_t> tilemap_hi) {
    if (sprites.empty() || tilemap.empty() || sprites.size() % 128 || tilemap.size() % 128 ||
        (!sprites_hi.empty() && sprites_hi.size() != sprites.size() / 2) ||
        (!tilemap_hi.empty() && tilemap_hi.size() != tilemap.size() / 2))
        return false;

    const size_t sprite_tiles = sprites.size() / 128;
    const size_t playfield_tiles = tilemap.size() / 128;
    m_sprite_tile_count = uint32_t(sprite_tiles);
    m_playfield_tile_count = uint32_t(playfield_tiles);
    m_sprite_tile_mask = (sprite_tiles & (sprite_tiles - 1)) ? 0 : uint32_t(sprite_tiles - 1);
    m_playfield_tile_mask =
        (playfield_tiles & (playfield_tiles - 1)) ? 0 : uint32_t(playfield_tiles - 1);
    m_decoded_sprites.resize(sprite_tiles * 256);
    m_decoded_tiles.resize(playfield_tiles * 256);

    // Decode 16x16 6bpp sprites: low 4 bpp packed LSB, high 2 bpp
    for (size_t c = 0; c < sprite_tiles; ++c) {
        const uint8_t *lo_tile = &sprites[c * 128];
        const uint8_t *hi_tile = sprites_hi.empty() ? nullptr : &sprites_hi[c * 64];
        uint8_t *dest = &m_decoded_sprites[c * 256];

        for (int y = 0; y < 16; ++y) {
            const uint8_t *lo_row = &lo_tile[ptrdiff_t(y) * 8];
            const uint8_t *hi_row = hi_tile ? &hi_tile[ptrdiff_t(y) * 4] : nullptr;
            uint8_t *dest_row = &dest[ptrdiff_t(y) * 16];

            for (int x = 0; x < 16; ++x) {
                uint8_t lo = (x & 1) ? (lo_row[x >> 1] >> 4) : (lo_row[x >> 1] & 0x0f);
                uint8_t hi = hi_row ? ((hi_row[x >> 2] >> (2 * (x & 3))) & 0x03) << 4 : 0;
                dest_row[x] = lo | hi;
            }
        }
    }

    // Decode 16x16 6bpp tilemaps: low 4 bpp packed LSB, high 2 bpp
    for (size_t c = 0; c < playfield_tiles; ++c) {
        const uint8_t *lo_tile = &tilemap[c * 128];
        const uint8_t *hi_tile = tilemap_hi.empty() ? nullptr : &tilemap_hi[c * 64];
        uint8_t *dest = &m_decoded_tiles[c * 256];

        for (int y = 0; y < 16; ++y) {
            const uint8_t *lo_row = &lo_tile[ptrdiff_t(y) * 8];
            const uint8_t *hi_row = hi_tile ? &hi_tile[ptrdiff_t(y) * 4] : nullptr;
            uint8_t *dest_row = &dest[ptrdiff_t(y) * 16];

            for (int x = 0; x < 16; ++x) {
                uint8_t lo = (x & 1) ? (lo_row[x >> 1] >> 4) : (lo_row[x >> 1] & 0x0f);
                uint8_t hi = 0;
                if (hi_row && x < 8) {
                    hi = (((hi_row[1] >> x) & 1) << 5) | (((hi_row[0] >> x) & 1) << 4);
                } else if (hi_row) {
                    hi = (((hi_row[3] >> (x - 8)) & 1) << 5) |
                         (((hi_row[2] >> (x - 8)) & 1) << 4);
                }
                dest_row[x] = lo | hi;
            }
        }
    }

    return true;
}

const uint8_t *GraphicsAssets::tile(std::span<const uint8_t> pixels, uint32_t code,
                                   uint32_t count, uint32_t mask) {
    const uint32_t index = mask ? code & mask : code % count;
    return pixels.data() + size_t(index) * 256;
}

const uint8_t *GraphicsAssets::sprite_tile(uint32_t code) const {
    return tile(m_decoded_sprites, code, m_sprite_tile_count, m_sprite_tile_mask);
}

const uint8_t *GraphicsAssets::playfield_tile(uint32_t code) const {
    return tile(m_decoded_tiles, code, m_playfield_tile_count, m_playfield_tile_mask);
}

} // namespace f3rt
