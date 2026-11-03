#pragma once
#include "renderer/game/scene.hpp"
#include "renderer/game/row_workers.hpp"
#include "f3rt/game_video.hpp"
#include "sprites/presentation.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {
class StateWriter;
class StateReader;

struct SpriteRasterOptions {
    GameVideoOptions geometry{}; // {} = native 432x256 plane
    bool flipped = false;        // flipscreen row phase
    bool accumulate = false;     // sprite trails: keep the existing plane contents
};
// Indexed sprite plane for `geometry`: 432x256 for the fixed native frame, width*height expanded.
constexpr size_t sprite_plane_size(GameVideoOptions geometry) {
    return geometry.expanded() ? size_t(geometry.width()) * geometry.height() : size_t(432) * 256;
}
// Indexed sprite plane from a latched list. Sprite 0 is the front-most;
// GameSprites::raster() delegates here, and CapturedFrame sprite lists use it
// for reference-scale planes.
void raster_sprites(std::span<const SceneSprite> sprites, uint8_t pen_mask,
                    std::span<const uint8_t> assets, std::span<uint16_t> output,
                    SpriteRasterOptions options = {});

// Per-plane-row sprite coverage written by raster_sprite_chunk: for every sprite layer, the exact
// set of picture columns whose plane value belongs to that layer. Render-only for the indexed
// compositor, which walks these bits instead of testing every plane texel. Bit c of a layer row is
// picture column c (plane column c + the plane's left margin: 46 for the native plane, 0 for an
// expanded one); each layer row ends in one guard word that stays zero.
struct SpriteCoverage {
    static constexpr unsigned LAYERS = 4;
    // Words per layer row for `geometry`: its picture columns plus the guard word.
    static constexpr unsigned words_for(GameVideoOptions geometry) {
        return (geometry.width() + 31) / 32 + 1;
    }
    unsigned words = 0;
    std::span<uint8_t> layers; // per plane row: bit i set when sprite layer i covers a column
    std::span<uint32_t> bits;  // per plane row, LAYERS layer rows of `words` words
    std::span<uint32_t> row(unsigned plane_row, unsigned layer) const {
        return bits.subspan((size_t(plane_row) * LAYERS + layer) * words, words);
    }
    // Zero one plane row's coverage.
    void clear(unsigned plane_row) const {
        layers[plane_row] = 0;
        std::fill_n(bits.begin() + ptrdiff_t(size_t(plane_row) * LAYERS * words),
                    size_t(LAYERS) * words, 0u);
    }
};

// Sprites bucketed once per frame by COMPOSE_CHUNK_ROWS-row plane chunks, so a worker rasterising
// and composing one chunk only walks the sprites that reach it. Buckets keep list order.
class SpriteBins {
public:
    // `sprites` indices are bucketed; the list itself is passed to raster_sprite_chunk.
    void bin(std::span<const SceneSprite> sprites, SpriteRasterOptions options);
    unsigned chunk_count() const { return unsigned(m_chunks.size()); }
    std::span<const uint16_t> chunk(unsigned chunk) const { return m_chunks[chunk]; }

private:
    std::vector<std::vector<uint16_t>> m_chunks;
};

// Clear the coverage of plane rows [chunk * COMPOSE_CHUNK_ROWS, ...) of the raster geometry, then
// draw the chunk's bucketed sprites into them (same order and same "first write wins" rule as
// raster_sprites). The plane itself is not cleared: a texel of the window holds the whole-plane
// raster's value where `coverage` has a bit and is stale everywhere else. `options.accumulate` is
// not supported.
void raster_sprite_chunk(const SpriteBins &bins, std::span<const SceneSprite> sprites,
                         unsigned chunk, uint8_t pen_mask, std::span<const uint8_t> assets,
                         std::span<uint16_t> plane, SpriteRasterOptions options,
                         SpriteCoverage &coverage);

class GameSprites {
public:
    GameSprites() { reset(); }

    void reset();
    // Decode the sprite display list from VRAM (0x00000, 0x10000 bytes) into the
    // next submission. Defined per game under games/<game>/video/.
    void decode(const VideoRam &vram);
    void latch();
    void raster(std::span<const uint8_t> assets, std::span<uint16_t> output,
                GameVideoOptions options = {}) const;
    std::span<const SceneSprite> sprites() const;
    // Presentation source for the next decode (null = none); set by GameVideo around decode.
    void set_presentation(const SpritePresentation *presentation) { m_presentation = presentation; }
    // Render-only: current sprites with unit splices applied, else sprites(). Same flip mirroring.
    std::span<const SceneSprite> presented_sprites() const;
    bool flipped() const;
    // The last decode produced a presented list (splices) and whether it hit MAX_PRESENTED_SPRITES.
    bool presented_valid() const { return m_presented_submitted_valid; }
    bool presented_overflow() const { return m_presented_overflow; }
    uint8_t pen_mask() const;
    bool trails() const;
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    friend class GameVideo;
    static constexpr size_t MAX_SPRITES = f3rt::MAX_HARDWARE_SPRITES;
    static constexpr size_t MAX_PRESENTED_SPRITES = f3rt::MAX_PRESENTED_SPRITES;

    std::array<SceneSprite, MAX_SPRITES> m_staging_sprites{};
    size_t m_staging_count = 0;

    std::array<SceneSprite, MAX_SPRITES> m_submitted_sprites{};
    size_t m_submitted_count = 0;

    std::array<SceneSprite, MAX_SPRITES> m_current_sprites{};
    size_t m_current_count = 0;
    bool m_current_flipped = false;
    uint8_t m_current_pen_mask = 15;
    bool m_current_trails = false;

    bool m_reg_flipped = false;
    uint8_t m_reg_pen_mask = 15;
    bool m_reg_trails = false;
    bool m_reg_bank = false;

    // Render-only splice view of the submitted/current lists (never serialized, so snapshots and
    // CRCs match the canonical decode). Valid only when the last decode had splices or tagged real entries.
    const SpritePresentation *m_presentation = nullptr;
    std::vector<SceneSprite> m_presented_submitted, m_presented_current;
    size_t m_presented_submitted_count = 0, m_presented_current_count = 0;
    bool m_presented_submitted_valid = false, m_presented_current_valid = false;
    bool m_presented_overflow = false; // last decode filled the presented list to capacity
};
static_assert(SceneSource<GameSprites>);

} // namespace f3rt
