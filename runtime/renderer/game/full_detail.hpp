#pragma once
#include "f3rt/renderer/game/enhancements.hpp"
#include "f3rt_video_cxx/video.h"
#include "renderer/game/scene.hpp"
#include <vector>

namespace f3rt {

class FullDetailStage final : public SpriteStage {
public:
    FullDetailStage(int visible_y, int visible_height);
    void present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) override;
    void reset() override;
    void report(std::ostream &out) const override;

private:
    rust::Box<rust_video::CommandwFullDetail> m_stage;
    rust::Vec<rust_video::SceneSprite> m_bridge_sprites;
};

}
