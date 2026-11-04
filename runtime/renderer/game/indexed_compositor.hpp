#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/compositor.hpp"
#include "renderer/game/row_workers.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/tiles.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {
// Byte order of one composed pixel. Argb8888 is the canonical 0xAARRGGBB word the CPU composers
// produce; Rgba8888 is the word the 3DS presenter's display transfer consumes (the ARGB word
// rotated left by eight bits with an opaque low byte), so no separate conversion pass exists.
enum class PixelFormat { Argb8888, Rgba8888 };

constexpr uint32_t to_pixel_format(uint32_t argb, PixelFormat format) {
    return format == PixelFormat::Rgba8888 ? (argb << 8) | 0xffu : argb;
}

// Palette lookup in output byte order: index -> pixel word, alpha forced opaque. The 15-bit table
// exists only when a row of the frame selects 15-bit palette words. Each table repeats its first
// GUARD entries after the last, so a palette base plus a pen (or a sprite plane value, which sits
// above 0x1000) can index it without wrapping first. The tables persist between frames: build()
// converts only the BLOCK-entry blocks whose source colours changed since the previous call.
struct IndexedPalette {
    static constexpr unsigned SIZE = 8192, GUARD = 64, BLOCK = 64;
    std::array<uint32_t, SIZE + GUARD> colors{}, colors15{};
    bool uses_15bit = false;

    void build(std::span<const uint32_t> palette, PixelFormat format, bool need_15bit);

private:
    std::array<uint32_t, SIZE> m_source{}; // the colours the tables were last converted from
    PixelFormat m_format = PixelFormat::Argb8888;
    bool m_built = false;        // `colors` holds the conversion of m_source in m_format
    bool m_built_15bit = false;  // `colors15` does too
};

// For each tile of the playfield tile ROM, which of its 16 pixel rows have a pen to draw. A pen byte
// carries 4 low pen bits plus two extra pen planes (bits 4 and 5) that a map cell may mask out, so
// each tile holds three row bitmasks: bit y of `low` is set when row y has a nonzero (pen & 0x0f),
// of `plane4` when some pen of the row has bit 4 set, of `plane5` for bit 5.
struct TileRows {
    uint16_t low, plane4, plane5;

    // Whether row `y` of the tile shows any pen under `pen_mask` (the cell's mask, at most 0x3f).
    bool visible(unsigned y, unsigned pen_mask) const {
        const unsigned rows = low | (plane4 & (0u - (pen_mask >> 4 & 1))) |
                              (plane5 & (0u - (pen_mask >> 5 & 1)));
        return rows >> y & 1;
    }
};

// The TileRows of one tile ROM, built the first time the ROM is seen (the ROM is immutable for as
// long as the same buffer is passed).
class TileRowTable {
public:
    // Scan `tile_pixels` (256 pen bytes per tile) unless this table was built from the same buffer.
    void ensure(std::span<const uint8_t> tile_pixels);
    std::span<const TileRows> tiles() const { return m_tiles; }

private:
    const uint8_t *m_source = nullptr;
    size_t m_size = 0;
    std::vector<TileRows> m_tiles;
};

// Per-frame counters filled by the row chunks (relaxed: workers increment concurrently).
struct IndexedFrameStats {
    std::atomic<uint32_t> fallback_rows{0}; // rows composed by the canonical row kernel
    std::atomic<uint32_t> special_px{0};    // pixels resolved by a real blend
    void reset() {
        fallback_rows.store(0, std::memory_order_relaxed);
        special_px.store(0, std::memory_order_relaxed);
    }
};

// Persistent working set of one indexed composer: the sprite plane (chunk-cleared by the raster),
// its per-row layer coverage bits and the sprite bins. Geometry changes reuse the buffers.
struct IndexedScratch {
    std::vector<uint16_t> plane;
    std::vector<uint8_t> coverage_layers;
    std::vector<uint32_t> coverage_bits;
    SpriteBins bins;
    IndexedPalette palette;
    TileRowTable tile_rows;
    std::array<uint8_t, 256> glyph_rows{};
    IndexedFrameStats stats;

    void configure(GameVideoOptions geometry);
    // The serial per-frame setup before the chunks run: the palette tables, the glyph row masks and
    // the tile row table for `scene`. Returns whether the frame uses the 15-bit palette table.
    bool prepare(const FrameScene &scene, PixelFormat format);
    // The coverage view of the buffers sized by configure(geometry).
    SpriteCoverage coverage(GameVideoOptions geometry) {
        return {SpriteCoverage::words_for(geometry), coverage_layers, coverage_bits};
    }
};

// One frame, immutable while the chunks run. `output` holds picture rows: row r of the presented
// picture at output[r * stride].
struct IndexedComposeJob {
    const FrameScene &scene;
    std::span<uint16_t> plane; // sprite plane at `geometry`; each chunk writes only its own rows
    SpriteCoverage coverage;   // per plane row layer coverage of this frame's raster
    const SpriteBins *bins = nullptr;
    std::span<const SceneSprite> sprites;
    uint8_t pen_mask = 15;
    std::span<const uint8_t> sprite_tiles;
    std::span<const TileRows> tile_rows; // TileRowTable::tiles() of scene.tile_pixels
    // Which glyph rows of the frame's text are not blank: bit y of entry g is set when row y of
    // glyph g has a nonzero texel. The compositor skips a blank glyph row without reading it.
    std::span<const uint8_t> glyph_rows;
    std::span<const uint32_t> palette;   // IndexedPalette::colors, guard entries included
    std::span<const uint32_t> palette15; // IndexedPalette::colors15 (empty when unused)
    std::span<uint32_t> output;
    size_t stride = 0;
    PixelFormat format = PixelFormat::Argb8888;
    GameVideoOptions geometry{}; // expanded scale-1 presentation (320x232 plus the border)
    IndexedFrameStats *stats = nullptr;
};

// Compose one COMPOSE_CHUNK_ROWS-row picture chunk on the caller: rasterise the chunk's sprites into
// its plane rows, record their coverage, then compose the rows. Pixel-identical to
// compose_scene_rows over the same rows. The chunk writes only its own plane rows and coverage
// rows of the job.
void compose_indexed_chunk(IndexedComposeJob &job, unsigned chunk) noexcept;
} // namespace f3rt
