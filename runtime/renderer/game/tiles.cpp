#include "renderer/game/tiles.hpp"
#include "f3rt/memory_map.hpp"
#include <bit>
#include <cstring>

namespace f3rt {

void GameTiles::reset() {
    for (auto &map : m_maps) map.fill(0);
    m_row_used.fill(0);
}

namespace {

constexpr uint32_t PF_BEGIN = OFFS_PLAYFIELDS; // PF0 data

} // namespace

void GameTiles::decode(const VideoRam &vram) {
    for (unsigned layer = 0; layer < m_maps.size(); ++layer) {
        const uint32_t base = PF_BEGIN + layer * PLAYFIELD_LAYER_SIZE;
        for (unsigned i = 0; i < m_maps[layer].size(); ++i) {
            const uint32_t at = base + i * 4;
            m_maps[layer][i] = uint32_t(vram.u16(at)) << 16 | vram.u16(at + 2);
        }
        m_row_used[layer] = 0;
        for (unsigned row = 0; row < 32; ++row)
            for (unsigned column = 0; column < 64; ++column)
                if (uint16_t(m_maps[layer][row * 64 + column])) {
                    m_row_used[layer] |= 1u << row;
                    break;
                }
    }
}

void FullResolutionAltMaps::reset() { *this = FullResolutionAltMaps{}; }

void FullResolutionAltMaps::update(const GameTiles &tiles, std::span<const uint8_t> tile_pixels) {
    if constexpr (f3rt::game_config::video.full_resolution_alt_maps) {
        if (tile_pixels.data() != m_tile_data || tile_pixels.size() != m_tile_size) {
            for (auto &layer : m_rows)
                for (auto &row : layer) row.valid = false;
            m_tile_data = tile_pixels.data();
            m_tile_size = tile_pixels.size();
        }
        for (unsigned l = 0; l < 2; ++l) {
            const auto main = tiles.cells(GameTiles::physical_map(2 + l, false));
            const auto alt = tiles.cells(GameTiles::physical_map(2 + l, true));
            for (unsigned r = 0; r < ROWS; ++r) {
                Row &row = m_rows[l][r];
                const uint32_t *m = main.data() + size_t(r) * COLUMNS,
                               *a = alt.data() + size_t(r) * COLUMNS;
                if (row.valid && !std::memcmp(row.main.data(), m, sizeof row.main) &&
                    !std::memcmp(row.alt.data(), a, sizeof row.alt))
                    continue;
                std::memcpy(row.main.data(), m, sizeof row.main);
                std::memcpy(row.alt.data(), a, sizeof row.alt);
                solve(row, tiles, tile_pixels, 2 + l, r);
                row.valid = true;
                ++m_solved_rows;
            }
        }
    }
}

int FullResolutionAltMaps::offset(unsigned layer, unsigned first_row, unsigned last_row) const {
    if constexpr (f3rt::game_config::video.full_resolution_alt_maps) {
        const auto &layer_rows = m_rows[layer - 2];
        Offsets common = layer_rows[first_row % ROWS].offsets;
        for (unsigned row = first_row + 1; row <= first_row + ((last_row - first_row) % ROWS);
             ++row)
            for (unsigned i = 0; i < common.size(); ++i)
                common[i] &= layer_rows[row % ROWS].offsets[i];
        for (unsigned i = 0; i < common.size(); ++i)
            if (common[i]) return int(i * 64 + unsigned(std::countr_zero(common[i])));
    }
    return -1;
}

void FullResolutionAltMaps::solve(Row &row, const GameTiles &tiles,
                                  std::span<const uint8_t> tile_pixels, unsigned layer,
                                  unsigned tile_row) {
    constexpr unsigned HALF = PF_MAP_WIDTH_PX / 2, LINES = 16;
    row.offsets = {};
    // The hardware never draws a row without tile codes (GameTiles::row_used); nothing to alias.
    if (!tiles.row_used(GameTiles::physical_map(layer, true), tile_row * LINES)) return;
    // Decoded samples (palette and flags) per line: the alt row, and the main row split by x parity.
    // A twin of alt(x) = main(2x + c) forces the alt row to repeat every half map.
    std::array<std::array<uint32_t, HALF>, LINES> alt, even, odd;
    std::array<uint32_t, PF_MAP_WIDTH_PX> samples;
    const auto decode = [&](bool alternate, unsigned line) {
        auto sampler =
            tiles.row_sampler(layer, int(tile_row * LINES + line), false, tile_pixels, alternate);
        for (unsigned x = 0; x < PF_MAP_WIDTH_PX; ++x) {
            const ScenePixel pixel = sampler.pixel(int(x));
            samples[x] = uint32_t(pixel.palette) | uint32_t(pixel.flags) << 16;
        }
    };
    for (unsigned line = 0; line < LINES; ++line) {
        decode(true, line);
        if (std::memcmp(samples.data(), samples.data() + HALF, HALF * sizeof(uint32_t)) != 0)
            return;
        std::memcpy(alt[line].data(), samples.data(), HALF * sizeof(uint32_t));
        decode(false, line);
        for (unsigned i = 0; i < HALF; ++i) {
            even[line][i] = samples[size_t(2) * i];
            odd[line][i] = samples[2 * i + 1];
        }
    }
    // c = 2m + parity: alt(x) == main(2x + c) <=> alt[x] == (parity ? odd : even)[(x + m) mod half].
    for (unsigned parity = 0; parity < 2; ++parity) {
        const auto &source = parity ? odd : even;
        for (unsigned m = 0; m < HALF; ++m) {
            bool exact = true;
            for (unsigned line = 0; exact && line < LINES; ++line)
                exact = !std::memcmp(alt[line].data(), source[line].data() + m,
                                     (HALF - m) * sizeof(uint32_t)) &&
                        !std::memcmp(alt[line].data() + (HALF - m), source[line].data(),
                                     m * sizeof(uint32_t));
            if (exact) {
                const unsigned c = 2 * m + parity;
                row.offsets[c / 64] |= uint64_t{1} << (c % 64);
            }
        }
    }
}
} // namespace f3rt
