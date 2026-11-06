#pragma once
#include "renderer/game/scene.hpp"
#include "renderer/game/row_bits.hpp"
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
    GameVideoOptions geometry{};
    bool flipped = false;
};
constexpr size_t sprite_plane_size(GameVideoOptions geometry) {
    return geometry.expanded() ? size_t(geometry.width()) * geometry.height() : size_t(432) * 256;
}
enum class SpriteTrails : bool { Off, On };

void raster_sprites(std::span<const SceneSprite> sprites, uint8_t pen_mask,
                    std::span<const uint8_t> assets, std::span<uint16_t> output,
                    SpriteRasterOptions options = {}, SpriteTrails trails = SpriteTrails::Off);

class SpriteCoverage {
public:
    static constexpr unsigned LAYERS = 4;
    static constexpr unsigned words_for(GameVideoOptions geometry) {
        return (geometry.width() + 31) / 32 + 1;
    }

    class Row {
    public:
        uint8_t layers() const { return *m_layers; }
        bool covers(unsigned layer) const { return (*m_layers >> layer) & 1u; }
        unsigned word_count() const { return m_words; }
        std::span<const uint32_t> columns(unsigned layer) const {
            return {m_bits + size_t(layer) * m_words, m_words};
        }

        uint32_t covered_word(unsigned word) const {
            uint32_t held = 0;
            for (unsigned mask = *m_layers; mask; mask &= mask - 1)
                held |= m_bits[size_t(__builtin_ctz(mask)) * m_words + word];
            return held;
        }
        [[gnu::always_inline]] uint32_t covered(unsigned column, unsigned len) const {
            uint32_t held = 0;
            for (unsigned mask = *m_layers; mask; mask &= mask - 1)
                held |= extract_columns(m_bits + size_t(__builtin_ctz(mask)) * m_words, int(column),
                                        len);
            return held;
        }

        void mark(unsigned layer, unsigned column, uint32_t bits) const {
            *m_layers |= uint8_t(1u << layer);
            set_columns(m_bits + size_t(layer) * m_words, int(column), bits);
        }
        void mark_word(unsigned layer, unsigned word, uint32_t bits) const {
            *m_layers |= uint8_t(1u << layer);
            m_bits[size_t(layer) * m_words + word] |= bits;
        }
        void clear() const {
            *m_layers = 0;
            std::fill_n(m_bits, size_t(LAYERS) * m_words, 0u);
        }

    private:
        friend class SpriteCoverage;
        Row(uint8_t *layer_flags, uint32_t *bits, unsigned words)
            : m_layers(layer_flags), m_bits(bits), m_words(words) {}

        uint8_t *m_layers;
        uint32_t *m_bits;
        unsigned m_words;
    };

    SpriteCoverage(GameVideoOptions geometry, std::span<uint8_t> layers, std::span<uint32_t> bits)
        : m_words(words_for(geometry)), m_layers(layers), m_bits(bits) {}

    Row row(unsigned plane_row) const {
        return Row(m_layers.data() + plane_row,
                   m_bits.data() + size_t(plane_row) * LAYERS * m_words, m_words);
    }

private:
    unsigned m_words;
    std::span<uint8_t> m_layers;
    std::span<uint32_t> m_bits;
};

class SpriteBins {
public:
    void bin(std::span<const SceneSprite> sprites, SpriteRasterOptions options);
    unsigned chunk_count() const { return unsigned(m_chunks.size()); }
    std::span<const uint16_t> chunk(unsigned chunk) const { return m_chunks[chunk]; }

private:
    std::vector<std::vector<uint16_t>> m_chunks;
};

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
