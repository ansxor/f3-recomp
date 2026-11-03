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

// Per-frame palette lookup in output byte order: index -> pixel word, alpha forced opaque.
// The 15-bit table exists only when a row of the frame selects 15-bit palette words.
struct IndexedPalette {
    std::array<uint32_t, 8192> colors{}, colors15{};
    bool uses_15bit = false;

    void build(std::span<const uint32_t> palette, PixelFormat format, bool need_15bit);
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
// its per-row coverage spans and the sprite bins. Geometry changes reuse the buffers.
struct IndexedScratch {
    std::vector<uint16_t> plane;
    std::vector<SpriteRowSpans::Row> span_rows;
    SpriteBins bins;
    IndexedPalette palette;
    IndexedFrameStats stats;

    void configure(GameVideoOptions geometry);
    SpriteRowSpans spans() { return {span_rows}; }
};

// One frame, immutable while the chunks run. `output` holds picture rows: row r of the presented
// picture at output[r * stride].
struct IndexedComposeJob {
    const FrameScene &scene;
    std::span<uint16_t> plane; // sprite plane at `geometry`; each chunk writes only its own rows
    SpriteRowSpans spans; // per plane row coverage of this frame's raster; one entry per plane row
    const SpriteBins *bins = nullptr;
    std::span<const SceneSprite> sprites;
    uint8_t pen_mask = 15;
    std::span<const uint8_t> sprite_tiles;
    std::span<const uint32_t> palette;   // IndexedPalette::colors
    std::span<const uint32_t> palette15; // IndexedPalette::colors15 (empty when unused)
    std::span<uint32_t> output;
    size_t stride = 0;
    PixelFormat format = PixelFormat::Argb8888;
    GameVideoOptions geometry{}; // expanded scale-1 presentation (320x232 plus the border)
    const TileRowOpacity *opacity = nullptr;
    IndexedFrameStats *stats = nullptr;
};

// True when any presented row of `scene` selects 15-bit palette words.
bool indexed_frame_uses_15bit(const FrameScene &scene) noexcept;

// Compose one COMPOSE_CHUNK_ROWS-row picture chunk on the caller: rasterise the chunk's sprites into
// its plane rows, record their coverage, then compose the rows. Pixel-identical to
// compose_scene_rows over the same rows for class-A rows, and to the canonical row kernel for rows
// where the first-visible-layer shortcut is not provably exact. The chunk writes only its own plane
// rows and span rows of the job.
void compose_indexed_chunk(IndexedComposeJob &job, unsigned chunk) noexcept;
} // namespace f3rt
