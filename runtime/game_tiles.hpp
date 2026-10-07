#pragma once
#include "game_scene.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Playfield tiles (PF0..PF3) snapshotted from FDP video RAM at VBSTART.
//
// Each layer is 2048 cells (64 columns x 32 rows of 16x16 texels) at graphics
// offset 0x10000 + layer * 0x2000. A cell is the raw 4-byte big-endian map
// entry; the snapshot is required because emulation keeps writing VRAM while a
// frame is composited or materialized lazily.
//
// Raw cell bit layout (identical in the GPU shader and GameTiles::RowSampler):
//   bytes 0-1, attributes (big-endian u16):
//     bits 0-8   palette code (palette base = code * 16)
//     bit  9     blend selector
//     bits 10-11 extra pen planes
//     bit  14    flip X
//     bit  15    flip Y
//     pen mask   = (((attributes >> 10) & 3 & ~attributes) << 4) | 15
//   bytes 2-3, tile code (big-endian u16): low 15 bits select the 256-byte tile.
class GameTiles {
public:
    // A compose layer/subrow owns this sampler; it decodes raw cells on demand.
    class RowSampler {
    public:
        RowSampler() = default;
        ScenePixel pixel(int x) {
            const unsigned wrapped_x = (unsigned(x) & 1023) ^ screen_flip_;
            const unsigned column = wrapped_x / 16;
            if (column != column_) {
                const uint32_t cell = cells_[column];
                const uint16_t attributes = uint16_t(cell >> 16);
                const uint16_t palette_code = attributes & 0x1ff;
                const unsigned ty = y_ ^ ((attributes & 0x8000) ? 15 : 0);
                pens_ = tiles_ + (uint16_t(cell) & 0x7fff) * 256 + ty * 16;
                palette_ = uint16_t(palette_code * 16);
                pen_mask_ = uint8_t((((attributes >> 10) & 3 & ~attributes) << 4) | 15);
                flip_x_ = (attributes & 0x4000) ? 15 : 0;
                blend_ = uint8_t((attributes >> 9) & 1);
                column_ = column;
            }
            const uint8_t pen = pens_[(wrapped_x & 15) ^ flip_x_] & pen_mask_;
            return {uint16_t(palette_ + pen), uint8_t((pen ? 0x10 : 0) | blend_)};
        }

    private:
        friend class GameTiles;
        const uint32_t *cells_ = nullptr;
        const uint8_t *tiles_ = nullptr;
        const uint8_t *pens_ = nullptr;
        unsigned y_ = 0, screen_flip_ = 0, column_ = 64;
        uint16_t palette_ = 0;
        uint8_t pen_mask_ = 0, flip_x_ = 0, blend_ = 0;
    };

    void reset();
    // Copy the four raw PF layers from video RAM at VBSTART. Defined per game.
    void decode(const VideoRam &vram);
#ifdef F3RT_VIDEO_WRITE_LOG
    // Debug builds: log stores from PCs outside the game's known list. Defined per game.
    void observe_write(uint32_t pc, uint32_t address, uint64_t frame);
#endif
    RowSampler row_sampler(unsigned layer, int y, bool flipped,
                           std::span<const uint8_t> tiles) const {
        const unsigned wrapped_y = (unsigned(y) & 511) ^ (flipped ? 511 : 0);
        RowSampler sampler;
        sampler.cells_ = maps_[layer].data() + (wrapped_y / 16) * 64;
        sampler.tiles_ = tiles.data();
        sampler.y_ = wrapped_y & 15;
        sampler.screen_flip_ = flipped ? 1023 : 0;
        return sampler;
    }
    ScenePixel playfield_pixel(unsigned layer, int x, int y, bool flipped,
                               std::span<const uint8_t> tiles) const {
        return row_sampler(layer, y, flipped, tiles).pixel(x);
    }

private:
    friend class GameVideo;
    // Raw 4-byte cell per 16x16 tile: attributes in the high word, code low.
    std::array<std::array<uint32_t, 2048>, 4> maps_{};
};

} // namespace f3rt
