#pragma once
#include "renderer/game/scene.hpp"
#include "f3rt/game_video.hpp"
#include "renderer/sprite_presentation.hpp"
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
    void set_presentation(const SpritePresentation *presentation) { presentation_ = presentation; }
    // Render-only: current sprites with unit splices applied, else sprites(). Same flip mirroring.
    std::span<const SceneSprite> presented_sprites() const;
    bool flipped() const;
    // The last decode produced a presented list (splices) and whether it hit kMaxPresentedSprites.
    bool presented_valid() const { return presented_submitted_valid_; }
    bool presented_overflow() const { return presented_overflow_; }
    uint8_t pen_mask() const;
    bool trails() const;
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    friend class GameVideo;
    static constexpr size_t kMaxSprites = max_hardware_sprites;
    static constexpr size_t kMaxPresentedSprites = max_presented_sprites;

    std::array<SceneSprite, kMaxSprites> staging_sprites_{};
    size_t staging_count_ = 0;

    std::array<SceneSprite, kMaxSprites> submitted_sprites_{};
    size_t submitted_count_ = 0;

    std::array<SceneSprite, kMaxSprites> current_sprites_{};
    size_t current_count_ = 0;
    bool current_flipped_ = false;
    uint8_t current_pen_mask_ = 15;
    bool current_trails_ = false;

    bool reg_flipped_ = false;
    uint8_t reg_pen_mask_ = 15;
    bool reg_trails_ = false;
    bool reg_bank_ = false;

    // Render-only splice view of the submitted/current lists (never serialized, so snapshots and
    // CRCs match the canonical decode). Valid only when the last decode had splices or tagged real entries.
    const SpritePresentation *presentation_ = nullptr;
    std::vector<SceneSprite> presented_submitted_, presented_current_;
    size_t presented_submitted_count_ = 0, presented_current_count_ = 0;
    bool presented_submitted_valid_ = false, presented_current_valid_ = false;
    bool presented_overflow_ = false; // last decode filled the presented list to capacity
};
static_assert(SceneSource<GameSprites> && Snapshotable<GameSprites>);

} // namespace f3rt
