#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt/memory_map.hpp"
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
    static constexpr unsigned COLUMNS = f3rt::game_config::video.extend ? 64 : 32;
    static constexpr unsigned CELLS_PER_MAP = COLUMNS * 32;
    static constexpr unsigned MAP_WIDTH_PX = COLUMNS * 16;
    static constexpr size_t MAP_STRIDE =
        f3rt::game_config::video.extend ? PLAYFIELD_LAYER_SIZE : PLAYFIELD_STANDARD_LAYER_SIZE;

    // A compose layer/subrow owns this sampler; it decodes raw cells on demand.
    class RowSampler {
    public:
        RowSampler() = default;
        ScenePixel pixel(int x) {
            const unsigned wrapped_x = (unsigned(x) & (MAP_WIDTH_PX - 1)) ^ m_screen_flip;
            const unsigned column = wrapped_x / 16;
            if (column != m_column) {
                const uint32_t cell = m_cells[column];
                const uint16_t attributes = uint16_t(cell >> 16);
                const uint16_t palette_code = attributes & 0x1ff;
                const unsigned ty = m_y ^ ((attributes & 0x8000) ? 15 : 0);
                m_pens = m_tiles + size_t(wrap_tile_index(uint16_t(cell), m_tile_count)) * 256 +
                         size_t(ty) * 16;
                m_palette = uint16_t(palette_code * 16);
                m_pen_mask = uint8_t((((attributes >> 10) & 3 & ~attributes) << 4) | 15);
                m_flip_x = (attributes & 0x4000) ? 15 : 0;
                m_blend = uint8_t((attributes >> 9) & 1);
                m_column = column;
            }
            // column is at most COLUMNS - 1, so the branch above always binds m_pens first;
            // m_column starts at COLUMNS, which no column matches.
            // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): m_pens bound above
            const uint8_t pen = m_pens[(wrapped_x & 15) ^ m_flip_x] & m_pen_mask;
            return {uint16_t(m_palette + pen), uint8_t((pen ? 0x10 : 0) | m_blend)};
        }

    private:
        friend class GameTiles;
        const uint32_t *m_cells = nullptr;
        const uint8_t *m_tiles = nullptr;
        const uint8_t *m_pens = nullptr;
        unsigned m_y = 0, m_screen_flip = 0, m_column = COLUMNS;
        uint32_t m_tile_count = 1;
        uint16_t m_palette = 0;
        uint8_t m_pen_mask = 0, m_flip_x = 0, m_blend = 0;
    };

    void reset();
    // Copy the raw PF layers from video RAM at VBSTART. Shared; defined in
    // runtime/renderer/game/tiles.cpp.
    void decode(const VideoRam &vram);
    // Physical map count: 4, or 6 with alternate PF2/PF3 maps.
    // In standard mode (!extend), PF2/PF3 can select alternate maps (maps 4/5) via line RAM bit 0x200.
    static constexpr unsigned MAP_COUNT =
        (!f3rt::game_config::video.extend || f3rt::game_config::video.extended_alt_maps) ? 6 : 4;
    static constexpr unsigned physical_map(unsigned layer, bool alt) {
        return layer + (alt ? 2 : 0);
    }
    RowSampler row_sampler(unsigned layer, int y, bool flipped, std::span<const uint8_t> tiles,
                           bool alt = false) const {
        const unsigned map = physical_map(layer, alt);
        const unsigned wrapped_y = (unsigned(y) & 511) ^ (flipped ? 511 : 0);
        RowSampler sampler;
        sampler.m_cells = m_maps[map].data() + size_t((wrapped_y / 16)) * COLUMNS;
        sampler.m_tiles = tiles.data();
        sampler.m_tile_count = uint32_t(tiles.size() / 256);
        sampler.m_y = wrapped_y & 15;
        sampler.m_screen_flip = flipped ? (MAP_WIDTH_PX - 1) : 0;
        return sampler;
    }
    // Raw cells of one layer (row-major COLUMNS x 32), for snapshot consumers.
    // Hardware row-usage cull (FDP update_row_usages): a map row whose 32+ tile code words are
    // all zero is not drawn at all, even when its attributes select visible pens. `source_y` is the
    // playfield source row (ScenePlayfield::source_y, 0..511), before global flip.
    bool row_used(unsigned map, unsigned source_y) const {
        return (m_row_used[map] >> ((source_y >> 4) & 31)) & 1;
    }
    // `map` is a physical map index (0..MAP_COUNT).
    std::span<const uint32_t, CELLS_PER_MAP> cells(unsigned map) const { return m_maps[map]; }
    std::span<uint32_t, CELLS_PER_MAP> cells(unsigned map) { return m_maps[map]; }
    ScenePixel playfield_pixel(unsigned layer, int x, int y, bool flipped,
                               std::span<const uint8_t> tiles, bool alt = false) const {
        return row_sampler(layer, y, flipped, tiles, alt).pixel(x);
    }

private:
    friend class GameVideo;
    // Raw 4-byte cell per 16x16 tile: attributes in the high word, code low.
    std::array<std::array<uint32_t, CELLS_PER_MAP>, MAP_COUNT> m_maps{};
    std::array<uint32_t, MAP_COUNT> m_row_used{}; // bit r: map row r has a nonzero tile code
};
static_assert(SceneSource<GameTiles>);

// Render-only (never serialized, never feeds emulation): which PF2/PF3 alternate-map tile rows are an
// exact X2/Y1 alias of the same row of the full-resolution main map. A game that draws distant floors
// from a horizontally half-scaled copy uploads both copies; the alias offset differs per upload
// context (the copies' origins differ), so it is solved from the maps, not configured.
//
// For tile row r of PF `layer` (2/3) the solved offsets are the c in [0, PF_MAP_WIDTH_PX)
// such that for every x in [0, PF_MAP_WIDTH_PX) and all 16 lines y of the row
//     alt(x, y) == main((2x + c) mod PF_MAP_WIDTH_PX, y)
// comparing the final decoded sample (GameTiles::RowSampler::pixel: palette+pen and flags), so tile
// codes, attributes and flips may differ between the maps. Rows whose alternate row holds no tile
// code (hardware row cull) and rows without any exact c have an empty set. Results are cached per row
// keyed on the raw cells of both maps, so only rows changed by an upload are solved again.
class FullResolutionAltMaps {
public:
    static constexpr unsigned ROWS = 32, COLUMNS = 64;
    using Offsets = std::array<uint64_t, PF_MAP_WIDTH_PX / 64>; // bit c: offset c is exact

    void reset();
    // Solve every PF2/PF3 tile row whose cells (or the tile ROM) changed since the previous update.
    // No-op unless game_config::video.full_resolution_alt_maps.
    void update(const GameTiles &tiles, std::span<const uint8_t> tile_pixels);
    // Smallest offset valid for every tile row from `first_row` through `last_row` (cyclic mod 32)
    // of PF `layer` (2 or 3); -1 when some row has none.
    int offset(unsigned layer, unsigned first_row, unsigned last_row) const;
    // Tile rows solved by update() since reset (cache misses).
    uint64_t solved_rows() const { return m_solved_rows; }

private:
    struct Row {
        std::array<uint32_t, COLUMNS> main{}, alt{}; // cache key: raw cells
        Offsets offsets{};
        bool valid = false; // key and offsets describe a solved row
    };
    void solve(Row &row, const GameTiles &tiles, std::span<const uint8_t> tile_pixels,
               unsigned layer, unsigned tile_row);
    std::array<std::array<Row, ROWS>, 2> m_rows{};
    const uint8_t *m_tile_data = nullptr;
    size_t m_tile_size = 0;
    uint64_t m_solved_rows = 0;
};

} // namespace f3rt
