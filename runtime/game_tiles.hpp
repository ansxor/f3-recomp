#pragma once
#include "game_scene.hpp"
#include "f3rt/cpu_abi.h"

namespace f3rt {
class StateWriter;
class StateReader;

class GameTiles {
    struct Cell {
        uint16_t tile = 0;
        uint16_t palette = 0;
        uint8_t pen_mask = 15;
        bool flip_x = false, flip_y = false, blend = false;
    };

public:
    // A compose layer/subrow owns this sampler; no cached data enters game state.
    class RowSampler {
    public:
        RowSampler() = default;
        ScenePixel pixel(int x) {
            const unsigned wrapped_x = (unsigned(x) & 1023) ^ screen_flip_;
            const unsigned column = wrapped_x / 16;
            if (column != column_) {
                const auto &cell = cells_[column];
                const unsigned ty = y_ ^ (cell.flip_y ? 15 : 0);
                pens_ = tiles_ + (cell.tile & 0x7fff) * 256 + ty * 16;
                palette_ = cell.palette;
                pen_mask_ = cell.pen_mask;
                flip_x_ = cell.flip_x ? 15 : 0;
                blend_ = uint8_t(cell.blend);
                column_ = column;
            }
            const uint8_t pen = pens_[(wrapped_x & 15) ^ flip_x_] & pen_mask_;
            return {uint16_t(palette_ + pen), uint8_t((pen ? 0x10 : 0) | blend_)};
        }

    private:
        friend class GameTiles;
        const Cell *cells_ = nullptr;
        const uint8_t *tiles_ = nullptr;
        const uint8_t *pens_ = nullptr;
        unsigned y_ = 0, screen_flip_ = 0, column_ = 64;
        uint16_t palette_ = 0;
        uint8_t pen_mask_ = 0, flip_x_ = 0, blend_ = 0;
    };
    void reset();
    void observe(GameMemory &memory, const f3_cpu &cpu);
    void observe_write(uint32_t pc, uint32_t address);
    bool supported(unsigned layer) const { return valid_[layer]; }
    uint32_t unsupported_pc(unsigned layer) const { return unsupported_[layer]; }
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
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    friend class GameVideo;
    std::array<std::array<Cell, 2048>, 4> maps_{};
    std::array<bool, 4> valid_{};
    std::array<uint32_t, 4> unsupported_{};
    void clear(unsigned layer);
    unsigned put(uint32_t destination, uint32_t tile, uint32_t pc);
};

} // namespace f3rt
