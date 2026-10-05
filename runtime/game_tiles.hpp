#pragma once
#include "game_scene.hpp"
#include "f3rt/cpu_abi.h"

namespace f3rt {
class StateWriter;
class StateReader;

class GameTiles {
public:
    void reset();
    void observe(GameMemory &memory, const f3_cpu &cpu);
    void observe_write(uint32_t pc, uint32_t address);
    bool supported(unsigned layer) const { return valid_[layer]; }
    uint32_t unsupported_pc(unsigned layer) const { return unsupported_[layer]; }
    ScenePixel playfield_pixel(unsigned layer, int x, int y, bool flipped,
                               std::span<const uint8_t> tiles) const {
        x &= 1023;
        y &= 511;
        if (flipped) { x = 1023 - x; y = 511 - y; }
        const auto &cell = maps_[layer][(y / 16) * 64 + x / 16];
        const unsigned tx = (x & 15) ^ (cell.flip_x ? 15 : 0);
        const unsigned ty = (y & 15) ^ (cell.flip_y ? 15 : 0);
        const uint8_t pen = tiles[(cell.tile & 0x7fff) * 256 + ty * 16 + tx] & cell.pen_mask;
        return {uint16_t(cell.palette + pen), uint8_t((pen ? 0x10 : 0) | unsigned(cell.blend))};
    }
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    friend class GameVideo;
    struct Cell {
        uint16_t tile = 0;
        uint16_t palette = 0;
        uint8_t pen_mask = 15;
        bool flip_x = false, flip_y = false, blend = false;
    };
    std::array<std::array<Cell, 2048>, 4> maps_{};
    std::array<bool, 4> valid_{};
    std::array<uint32_t, 4> unsupported_{};
    void clear(unsigned layer);
    unsigned put(uint32_t destination, uint32_t tile, uint32_t pc);
};

} // namespace f3rt
