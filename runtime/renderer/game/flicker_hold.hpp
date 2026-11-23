#pragma once
#include "f3rt/renderer/game/enhancements.hpp"
#include "f3rt_video_cxx/video.h"
#include "generated_config/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include <vector>

namespace f3rt {

// Flicker-hold sprite stage (`rust_video::FlickerHold`).
class FlickerHoldStage final : public SpriteStage {
public:
    explicit FlickerHoldStage(const game_config::FlickerHold &config);
    void present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) override;
    void reset() override;
    void report(std::ostream &out) const override;

private:
    rust::Box<rust_video::FlickerHold> m_stage;
};

} // namespace f3rt
