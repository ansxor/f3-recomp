#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/scene.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Playfield tiles (PF0..PF3, plus the PF2/PF3 alternate maps when the game's
// extended_alt_maps layout is configured) snapshotted from FDP video RAM at VBSTART.
//
// Each layer is 2048 cells (64 columns x 32 rows of 16x16 texels) at graphics
// offset 0x10000 + layer * 0x2000. A cell is the raw 4-byte big-endian map
// entry; the snapshot is required because emulation keeps writing VRAM while a
// frame is composited or materialized lazily.
//
// Physical maps 0..3 are PF0..PF3; maps 4/5 are the alternates of PF2/PF3 at
// 0x18000/0x1a000, selected per scanline (ScenePlayfield::alt_map).
//
// Raw cell bit layout (identical in the GPU shader and GameTiles::RowSampler):
//   bytes 0-1, attributes (big-endian u16):
//     bits 0-8   palette code (palette base = code * 16)
//     bit  9     blend selector
//     bits 10-11 extra pen planes
//     bit  14    flip X
//     bit  15    flip Y
//     pen mask   = (((attributes >> 10) & 3 & ~attributes) << 4) | 15
//   bytes 2-3, tile code (big-endian u16): the full 16-bit word, wrapped to the PF ROM tile count
//   (wrap_tile_index; MAME y-ack/fdp-collapse tc0630fdp.cpp wraps modulo the gfx element count).
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
                pens_ = tiles_ + size_t(wrap_tile_index(uint16_t(cell), tile_count_)) * 256 + ty * 16;
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
        uint32_t tile_count_ = 1;
        uint16_t palette_ = 0;
        uint8_t pen_mask_ = 0, flip_x_ = 0, blend_ = 0;
    };

    void reset();
    // Copy the four raw PF layers from video RAM at VBSTART. Shared; defined in
    // runtime/renderer/game/tiles.cpp.
    void decode(const VideoRam &vram);
    // Physical map count: 4, or 6 with alternate PF2/PF3 maps.
    static constexpr unsigned map_count = f3rt::game_config::video.extended_alt_maps ? 6 : 4;
    static constexpr unsigned physical_map(unsigned layer, bool alt) { return layer + (alt ? 2 : 0); }
    RowSampler row_sampler(unsigned layer, int y, bool flipped,
                           std::span<const uint8_t> tiles, bool alt = false) const {
        const unsigned map = physical_map(layer, alt);
        const unsigned wrapped_y = (unsigned(y) & 511) ^ (flipped ? 511 : 0);
        RowSampler sampler;
        sampler.cells_ = maps_[map].data() + (wrapped_y / 16) * 64;
        sampler.tiles_ = tiles.data();
        sampler.tile_count_ = uint32_t(tiles.size() / 256);
        sampler.y_ = wrapped_y & 15;
        sampler.screen_flip_ = flipped ? 1023 : 0;
        return sampler;
    }
    // Raw cells of one layer (2048, row-major 64x32), for snapshot consumers.
    // Hardware row-usage cull (FDP update_row_usages): a map row whose 32+ tile code words are
    // all zero is not drawn at all, even when its attributes select visible pens. `source_y` is the
    // playfield source row (ScenePlayfield::source_y, 0..511), before global flip.
    bool row_used(unsigned map, unsigned source_y) const { return (row_used_[map] >> ((source_y >> 4) & 31)) & 1; }
    // `map` is a physical map index (0..map_count).
    std::span<const uint32_t, 2048> cells(unsigned map) const { return maps_[map]; }
    std::span<uint32_t, 2048> cells(unsigned map) { return maps_[map]; }
    ScenePixel playfield_pixel(unsigned layer, int x, int y, bool flipped,
                               std::span<const uint8_t> tiles, bool alt = false) const {
        return row_sampler(layer, y, flipped, tiles, alt).pixel(x);
    }

private:
    friend class GameVideo;
    // Raw 4-byte cell per 16x16 tile: attributes in the high word, code low.
    std::array<std::array<uint32_t, 2048>, map_count> maps_{};
    std::array<uint32_t, map_count> row_used_{}; // bit r: map row r has a nonzero tile code
};
static_assert(SceneSource<GameTiles>);

// Render-only (never serialized, never feeds emulation): which PF2/PF3 alternate-map tile rows are an
// exact X2/Y1 alias of the same row of the full-resolution main map. A game that draws distant floors
// from a horizontally half-scaled copy uploads both copies; the alias offset differs per upload
// context (the copies' origins differ), so it is solved from the maps, not configured.
//
// For tile row r of PF `layer` (2/3) the solved offsets are the c in [0, pf_map_width_px)
// such that for every x in [0, pf_map_width_px) and all 16 lines y of the row
//     alt(x, y) == main((2x + c) mod pf_map_width_px, y)
// comparing the final decoded sample (GameTiles::RowSampler::pixel: palette+pen and flags), so tile
// codes, attributes and flips may differ between the maps. Rows whose alternate row holds no tile
// code (hardware row cull) and rows without any exact c have an empty set. Results are cached per row
// keyed on the raw cells of both maps, so only rows changed by an upload are solved again.
class FullResolutionAltMaps {
public:
    static constexpr unsigned rows = 32, columns = 64;
    using Offsets = std::array<uint64_t, pf_map_width_px / 64>; // bit c: offset c is exact

    void reset();
    // Solve every PF2/PF3 tile row whose cells (or the tile ROM) changed since the previous update.
    // No-op unless game_config::video.full_resolution_alt_maps.
    void update(const GameTiles &tiles, std::span<const uint8_t> tile_pixels);
    // Smallest offset valid for every tile row from `first_row` through `last_row` (cyclic mod 32)
    // of PF `layer` (2 or 3); -1 when some row has none.
    int offset(unsigned layer, unsigned first_row, unsigned last_row) const;
    // Tile rows solved by update() since reset (cache misses).
    uint64_t solved_rows() const { return solved_rows_; }

private:
    struct Row {
        std::array<uint32_t, columns> main{}, alt{}; // cache key: raw cells
        Offsets offsets{};
        bool valid = false;                          // key and offsets describe a solved row
    };
    void solve(Row &row, const GameTiles &tiles, std::span<const uint8_t> tile_pixels,
               unsigned layer, unsigned tile_row);
    std::array<std::array<Row, rows>, 2> rows_{};
    const uint8_t *tile_data_ = nullptr;
    size_t tile_size_ = 0;
    uint64_t solved_rows_ = 0;
};

} // namespace f3rt
