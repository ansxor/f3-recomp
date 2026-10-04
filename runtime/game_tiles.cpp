#include "game_tiles.hpp"

namespace f3rt {
namespace {
constexpr std::array<uint32_t, 9> tile_hooks{
    0x55c2, 0x5614, 0x56ae, 0x5a22, 0x5a5e, 0x5a9a, 0x5af4, 0x9bcea, 0x9ec4e
};
}

std::span<const uint32_t> GameTiles::hooks() { return tile_hooks; }

void GameTiles::reset() {
    for (auto &map : maps_) map.fill({});
    valid_.fill(false);
    unsupported_.fill(0);
}

void GameTiles::clear(unsigned layer) {
    maps_[layer].fill({});
    valid_[layer] = true;
    unsupported_[layer] = 0;
}

unsigned GameTiles::put(uint32_t destination, uint32_t value, uint32_t pc) {
    destination &= 0xffffff;
    if (destination < 0x610000 || destination >= 0x618000) return 0;
    const unsigned layer = (destination - 0x610000) / 0x2000;
    if (destination & 3) {
        valid_[layer] = false;
        unsupported_[layer] = pc;
        return 1u << layer;
    }
    auto &cell = maps_[layer][(destination & 0x1fff) / 4];
    const uint16_t attributes = uint16_t(value >> 16);
    cell.tile = uint16_t(value);
    cell.palette = uint16_t((attributes & 0x1ff) * 16);
    cell.pen_mask = uint8_t((((attributes >> 10) & 3 & ~attributes) << 4) | 15);
    cell.blend = (attributes & 0x0200) != 0;
    cell.flip_x = (attributes & 0x4000) != 0;
    cell.flip_y = (attributes & 0x8000) != 0;
    return 1u << layer;
}

void GameTiles::observe(GameMemory &memory, const f3_cpu &cpu) {
    switch (cpu.pc) {
    case 0x5a22: clear(0); return;
    case 0x5a5e: clear(1); return;
    case 0x5a9a: clear(2); return;
    case 0x5af4: clear(3); return;
    case 0x9bcea:
        for (unsigned layer = 0; layer < maps_.size(); ++layer) clear(layer);
        return;
    case 0x9ec4e:
        // Selection-screen side strips: one game-selected tile in two 4x15 grids.
        for (unsigned y = 0; y < 15; ++y) {
            for (unsigned x = 0; x < 4; ++x) {
                const uint32_t tile = 0x0c800000u | uint16_t(cpu.d[0]);
                put(0x610050 + y * 256 + x * 4, tile, cpu.pc);
                put(0x6100f0 + y * 256 + x * 4, tile, cpu.pc);
            }
        }
        return;
    case 0x55c2: case 0x5614: case 0x56ae: break;
    default: return;
    }

    // C stack arguments before LINK: descriptor pointer, destination, optional
    // palette word and attribute XOR. The descriptor remains game ROM/work RAM.
    uint32_t source = memory.u32(cpu.a[7] + 4);
    uint32_t destination = memory.u32(cpu.a[7] + 8);
    const uint16_t rows = memory.u16(source);
    const uint16_t columns = memory.u16(source + 2);
    source += 4;
    uint32_t attributes = 0;
    if (cpu.pc != 0x56ae) {
        attributes = memory.u32(cpu.a[7] + (cpu.pc == 0x55c2 ? 14 : 12)) & 0xffff0000u;
        if (cpu.pc == 0x55c2)
            attributes = (attributes & ~0x01ff0000u) | (uint32_t(memory.u16(cpu.a[7] + 12) & 0x1ff) << 16);
    }

    int column_step = 4, row_step = 256;
    if (attributes & 0x40000000u) {
        column_step = -4;
        // ROM $5680/$56a0 use D1.W, without index scaling. Do not "fix" it.
        destination += int16_t(columns) - 4;
    }
    if (attributes & 0x80000000u) {
        row_step = -256;
        destination += int16_t(uint16_t((rows - 1) << 8));
    }

    const bool erase = cpu.pc == 0x56ae;
    // The copy loops are do/while SUBQ.W/BGT; the erase helper tests before
    // writing. Preserve their different zero and signed-word boundary behavior.
    const unsigned height = erase ? (rows <= 0x8000 ? rows : 0) : (rows && rows <= 0x8000 ? rows : 1);
    const unsigned width = erase ? (columns <= 0x8000 ? columns : 0) : (columns && columns <= 0x8000 ? columns : 1);
    unsigned affected = 0;
    for (unsigned y = 0; y < height; ++y) {
        uint32_t cursor = destination;
        for (unsigned x = 0; x < width; ++x) {
            const uint32_t value = erase ? 0 : memory.u32(source) ^ attributes;
            if (!erase) source += 4;
            affected |= put(cursor, value, cpu.pc);
            cursor += column_step;
        }
        destination += row_step;
    }
    if (!memory.supported) {
        for (unsigned layer = 0; layer < maps_.size(); ++layer) {
            if (affected & (1u << layer)) {
                valid_[layer] = false;
                unsupported_[layer] = cpu.pc;
            }
        }
    }
}

void GameTiles::observe_write(uint32_t pc, uint32_t address) {
    if (address < 0x610000 || address >= 0x618000) return;
    switch (pc) {
    case 0x55fc: case 0x5646: case 0x56d6:
    case 0x5a2e: case 0x5a6a: case 0x5aa6: case 0x5b00:
    case 0x9bd08: case 0x9bd0a: case 0x9bd0c: case 0x9bd0e:
    case 0x9ec66: case 0x9ec6a: case 0x9ec6e: case 0x9ec72:
        return;
    default:
        const unsigned layer = (address - 0x610000) / 0x2000;
        if (valid_[layer] || !unsupported_[layer]) unsupported_[layer] = pc;
        valid_[layer] = false;
    }
}

ScenePixel GameTiles::playfield_pixel(unsigned layer, int x, int y, bool flipped,
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

} // namespace f3rt
