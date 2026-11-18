#pragma once
#include "f3rt/renderer/game/enhancements.hpp"
#include "generated_config/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include <unordered_map>
#include <vector>

namespace f3rt {

class FlickerHoldStage final : public SpriteStage {
public:
    explicit FlickerHoldStage(const game_config::FlickerHold &config) : m_config(config) {}
    void present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) override;
    void reset() override;
    void report(std::ostream &out) const override;

private:
    struct Stats {
        uint64_t held_frames = 0, drops = 0, anchor_missing = 0;
    };
    struct Held {
        uint16_t game_frame = 0;
        std::vector<SceneSprite> sprites;
        int32_t anchor_x = 0, anchor_y = 0;
        bool has_anchor_root = false;
    };
    game_config::FlickerHold m_config;
    std::unordered_map<uint64_t, Held> m_groups;
    Stats m_stats;
    bool m_flipped = false;
};

}
