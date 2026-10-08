#include "renderer/game/tiles.hpp"
#include <bit>
#include <cstring>

namespace f3rt {

void GameTiles::reset() {
    for (auto &map : maps_) map.fill(0);
    row_used_.fill(0);
}

namespace {

constexpr uint32_t pf_begin = 0x10000; // PF0 data

} // namespace

void GameTiles::decode(const VideoRam &vram) {
    for (unsigned layer = 0; layer < maps_.size(); ++layer) {
        const uint32_t base = pf_begin + layer * 0x2000;
        for (unsigned i = 0; i < maps_[layer].size(); ++i) {
            const uint32_t at = base + i * 4;
            maps_[layer][i] = uint32_t(vram.u16(at)) << 16 | vram.u16(at + 2);
        }
        row_used_[layer] = 0;
        for (unsigned row = 0; row < 32; ++row)
            for (unsigned column = 0; column < 64; ++column)
                if (uint16_t(maps_[layer][row * 64 + column])) { row_used_[layer] |= 1u << row; break; }
    }
}

void FullResolutionAltMaps::reset() {
    *this = FullResolutionAltMaps{};
}

void FullResolutionAltMaps::update(const GameTiles &tiles, std::span<const uint8_t> tile_pixels) {
    if constexpr (f3rt::game_config::video.full_resolution_alt_maps) {
        if (tile_pixels.data() != tile_data_ || tile_pixels.size() != tile_size_) {
            for (auto &layer : rows_)
                for (auto &row : layer) row.valid = false;
            tile_data_ = tile_pixels.data();
            tile_size_ = tile_pixels.size();
        }
        for (unsigned l = 0; l < 2; ++l) {
            const auto main = tiles.cells(GameTiles::physical_map(2 + l, false));
            const auto alt = tiles.cells(GameTiles::physical_map(2 + l, true));
            for (unsigned r = 0; r < rows; ++r) {
                Row &row = rows_[l][r];
                const uint32_t *m = main.data() + r * columns, *a = alt.data() + r * columns;
                if (row.valid && !std::memcmp(row.main.data(), m, sizeof row.main) &&
                    !std::memcmp(row.alt.data(), a, sizeof row.alt)) continue;
                std::memcpy(row.main.data(), m, sizeof row.main);
                std::memcpy(row.alt.data(), a, sizeof row.alt);
                solve(row, tiles, tile_pixels, 2 + l, r);
                row.valid = true;
                ++solved_rows_;
            }
        }
    }
}

int FullResolutionAltMaps::offset(unsigned layer, unsigned first_row, unsigned last_row) const {
    if constexpr (f3rt::game_config::video.full_resolution_alt_maps) {
        const auto &layer_rows = rows_[layer - 2];
        Offsets common = layer_rows[first_row % rows].offsets;
        for (unsigned row = first_row + 1; row <= first_row + ((last_row - first_row) % rows); ++row)
            for (unsigned i = 0; i < common.size(); ++i) common[i] &= layer_rows[row % rows].offsets[i];
        for (unsigned i = 0; i < common.size(); ++i)
            if (common[i]) return int(i * 64 + unsigned(std::countr_zero(common[i])));
    }
    return -1;
}

void FullResolutionAltMaps::solve(Row &row, const GameTiles &tiles, std::span<const uint8_t> tile_pixels,
                                  unsigned layer, unsigned tile_row) {
    constexpr unsigned half = pf_map_width_px / 2, lines = 16;
    row.offsets = {};
    // The hardware never draws a row without tile codes (GameTiles::row_used); nothing to alias.
    if (!tiles.row_used(GameTiles::physical_map(layer, true), tile_row * lines)) return;
    // Decoded samples (palette and flags) per line: the alt row, and the main row split by x parity.
    // A twin of alt(x) = main(2x + c) forces the alt row to repeat every half map.
    std::array<std::array<uint32_t, half>, lines> alt, even, odd;
    std::array<uint32_t, pf_map_width_px> samples;
    const auto decode = [&](bool alternate, unsigned line) {
        auto sampler = tiles.row_sampler(layer, int(tile_row * lines + line), false, tile_pixels, alternate);
        for (unsigned x = 0; x < pf_map_width_px; ++x) {
            const ScenePixel pixel = sampler.pixel(int(x));
            samples[x] = uint32_t(pixel.palette) | uint32_t(pixel.flags) << 16;
        }
    };
    for (unsigned line = 0; line < lines; ++line) {
        decode(true, line);
        if (std::memcmp(samples.data(), samples.data() + half, half * sizeof(uint32_t))) return;
        std::memcpy(alt[line].data(), samples.data(), half * sizeof(uint32_t));
        decode(false, line);
        for (unsigned i = 0; i < half; ++i) {
            even[line][i] = samples[2 * i];
            odd[line][i] = samples[2 * i + 1];
        }
    }
    // c = 2m + parity: alt(x) == main(2x + c) <=> alt[x] == (parity ? odd : even)[(x + m) mod half].
    for (unsigned parity = 0; parity < 2; ++parity) {
        const auto &source = parity ? odd : even;
        for (unsigned m = 0; m < half; ++m) {
            bool exact = true;
            for (unsigned line = 0; exact && line < lines; ++line)
                exact = !std::memcmp(alt[line].data(), source[line].data() + m, (half - m) * sizeof(uint32_t)) &&
                        !std::memcmp(alt[line].data() + (half - m), source[line].data(), m * sizeof(uint32_t));
            if (exact) {
                const unsigned c = 2 * m + parity;
                row.offsets[c / 64] |= uint64_t{1} << (c % 64);
            }
        }
    }
}
} // namespace f3rt
