#pragma once
#include "game_scene.hpp"
#include "f3rt/game_video.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {
class StateWriter;
class StateReader;

class GameSprites {
public:
    GameSprites() { reset(); }

    void reset();
    // Decode the sprite display list from VRAM (0x00000, 0x10000 bytes) into the
    // next submission. Defined per game under games/<game>/video/sprites.cpp.
    void decode(const VideoRam &vram);
#ifdef F3RT_VIDEO_WRITE_LOG
    void observe_write(uint32_t pc, uint32_t address, uint64_t frame);
#endif
    void latch();
    void raster(std::span<const uint8_t> assets, std::span<uint16_t> output,
                GameVideoOptions options = {}) const;
    std::span<const SceneSprite> sprites() const;
    bool flipped() const;
    uint8_t pen_mask() const;
    bool trails() const;
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    friend class GameVideo;
    static constexpr size_t kMaxSprites = 1024;

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
};

} // namespace f3rt
