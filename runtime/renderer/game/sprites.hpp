#pragma once
#include "renderer/game/scene.hpp"
#include "f3rt/game_video.hpp"
#include "sprites/presentation.hpp"
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
// Indexed sprite plane from a latched list. Sprite 0 is the front-most;
// GameSprites::raster() delegates here, and CapturedFrame sprite lists use it
// for reference-scale planes.
void raster_sprites(std::span<const SceneSprite> sprites, uint8_t pen_mask,
                    std::span<const uint8_t> assets, std::span<uint16_t> output,
                    SpriteRasterOptions options = {});

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
