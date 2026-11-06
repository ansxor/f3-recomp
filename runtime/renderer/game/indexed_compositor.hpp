#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/frame_scene.hpp"
#include "renderer/game/row_workers.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/tiles.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {
enum class ComposeMode { Parallel, Serial };
enum class SpriteList { Presented, Canonical };

// Byte order of one composed pixel. Argb8888 is the canonical 0xAARRGGBB word the CPU composers
// produce; Rgba8888 is the word the 3DS presenter's display transfer consumes (the ARGB word
// rotated left by eight bits with an opaque low byte), so no separate conversion pass exists.
enum class PixelFormat { Argb8888, Rgba8888 };

constexpr uint32_t to_pixel_format(uint32_t argb, PixelFormat format) {
    return format == PixelFormat::Rgba8888 ? (argb << 8) | 0xffu : argb;
}

struct IndexedPalette {
    static constexpr unsigned SIZE = 8192, GUARD = 64, BLOCK = 64;
    std::array<uint32_t, SIZE + GUARD> colors{}, colors15{};
    bool uses_15bit = false;

    void build(std::span<const uint32_t> palette, PixelFormat format, bool need_15bit);

private:
    std::array<uint32_t, SIZE> m_source{};
    PixelFormat m_format = PixelFormat::Argb8888;
    bool m_built = false;
    bool m_built_15bit = false;
};

struct TileRows {
    uint16_t low, plane4, plane5;

    bool visible(unsigned y, unsigned pen_mask) const {
        const unsigned rows = low | (plane4 & (0u - (pen_mask >> 4 & 1))) |
                              (plane5 & (0u - (pen_mask >> 5 & 1)));
        return rows >> y & 1;
    }
};

class TileRowTable {
public:
    TileRowTable() = default;
    explicit TileRowTable(std::span<const uint8_t> tile_pixels);
    std::span<const uint8_t> pixels() const { return m_pixels; }
    std::span<const TileRows> rows() const { return m_rows; }

private:
    std::span<const uint8_t> m_pixels;
    std::vector<TileRows> m_rows;
};

struct IndexedFrameStats {
    std::atomic<uint32_t> special_px{0};
    void reset() { special_px.store(0, std::memory_order_relaxed); }
};

struct IndexedScratch {
    std::vector<uint16_t> plane;
    std::vector<uint8_t> coverage_layers;
    std::vector<uint32_t> coverage_bits;
    SpriteBins bins;
    IndexedPalette palette;
    std::array<uint8_t, 256> glyph_rows{};
    IndexedFrameStats stats;

    void configure(GameVideoOptions geometry);
    bool prepare(const FrameScene &scene, PixelFormat format);
    SpriteCoverage coverage(GameVideoOptions geometry) {
        return {geometry, coverage_layers, coverage_bits};
    }
};

struct IndexedComposeJob {
    const FrameScene &scene;
    std::span<uint16_t> plane;
    SpriteCoverage coverage;
    const SpriteBins *bins = nullptr;
    std::span<const SceneSprite> sprites;
    uint8_t pen_mask = 15;
    std::span<const uint8_t> sprite_tiles;
    std::span<const TileRows> tile_rows;
    std::span<const uint8_t> glyph_rows;
    std::span<const uint32_t> palette;
    std::span<const uint32_t> palette15;
    std::span<uint32_t> output;
    size_t stride = 0;
    PixelFormat format = PixelFormat::Argb8888;
    GameVideoOptions geometry{};
    IndexedFrameStats *stats = nullptr;
};

void compose_indexed_chunk(IndexedComposeJob &job, unsigned chunk) noexcept;
} // namespace f3rt
