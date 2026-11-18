#pragma once
#include "generated_config/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace f3rt {

class SpriteGroupTracker;

struct FlickerFrame {
    const SpriteGroupTracker *groups;
    uint16_t game_frame;
    bool flipped;

    static std::optional<FlickerFrame> read(const SpriteGroupTracker &groups, bool flipped);
};

class FlickerHoldStage {
public:
    struct Stats {
        uint64_t held_frames = 0, drops = 0, anchor_missing = 0;
    };

    explicit FlickerHoldStage(std::span<const game_config::FlickerHold> holds = {});
    bool active() const { return !m_holds.empty(); }
    void present(std::span<const SceneSprite> latched, std::optional<FlickerFrame> frame);
    std::span<const SceneSprite> sprites() const { return m_presented; }
    void reset();
    void report(std::ostream &out) const;

private:
    struct Held {
        uint16_t game_frame = 0;
        std::vector<SceneSprite> sprites;
        int32_t anchor_x = 0, anchor_y = 0;
        bool has_anchor_root = false;
    };
    struct Hold {
        game_config::FlickerHold config;
        std::unordered_map<uint64_t, Held> groups;
        Stats stats;
    };

    std::vector<Hold> m_holds;
    std::vector<SceneSprite> m_presented;
    bool m_flipped = false;
};

}
