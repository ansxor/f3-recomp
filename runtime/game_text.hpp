#pragma once
#include "game_scene.hpp"
#include "f3rt/cpu_abi.h"

namespace f3rt {
class StateWriter;
class StateReader;

class GameText {
public:
    GameText() { reset(); }
    void reset();
    void observe(GameMemory &memory, const f3_cpu &cpu);
    void observe_write(uint32_t pc, uint32_t address);
    bool supported() const;
    uint32_t unsupported_pc() const { return unsupported_pc_; }
    ScenePixel pixel(int x, int y, bool flipped) const {
        x &= 511;
        y &= 511;
        if (flipped) { x = 511 - x; y = 511 - y; }
        const auto &cell = cells_[(y / 8) * 64 + x / 8];
        const unsigned tx = (x & 7) ^ (cell.flip_x ? 7 : 0);
        const unsigned ty = (y & 7) ^ (cell.flip_y ? 7 : 0);
        const uint8_t pen = glyphs_[cell.tile * 64 + ty * 8 + tx];
        return {uint16_t(cell.palette * 16 + pen), uint8_t(pen ? 0x10 : 0)};
    }
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    friend class GameVideo;
    struct Cell {
        uint8_t tile = 0, palette = 0;
        bool flip_x = false, flip_y = false;
    };
    std::array<Cell, 4096> cells_{};
    std::array<uint8_t, 256 * 64> glyphs_{};
    std::array<uint8_t, 256> glyph_rows_{};
    std::array<uint16_t, 256> references_{};
    bool map_valid_ = false;
    uint32_t unsupported_pc_ = 0;
    void put(uint32_t destination, uint16_t value);
    void clear();
    void glyph_row(uint32_t destination, uint32_t value);
    void solid_glyph(unsigned tile, uint8_t pen);
    void glyph_mask(uint32_t destination, uint16_t mask, bool set);
};

} // namespace f3rt
