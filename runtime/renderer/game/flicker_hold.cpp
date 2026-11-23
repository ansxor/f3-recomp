#include "renderer/game/flicker_hold.hpp"
#include "f3rt/bridge_video_host.hpp"
#include "sprites/groups.hpp"
#include <ostream>

namespace f3rt {

FlickerHoldStage::FlickerHoldStage(const game_config::FlickerHold &config)
    : m_stage(rust_video::new_flicker_hold(
          config.name, config.group_class, config.anchor_class, config.period,
          config.follow == game_config::FlickerHold::Follow::Self)) {}

void FlickerHoldStage::present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) {
    rust::Vec<rust_video::SceneSprite> list;
    list.reserve(sprites.size());
    for (const auto &sprite : sprites) {
        list.push_back({sprite.x,        sprite.y,       sprite.scale_x, sprite.scale_y,
                        sprite.tile,     sprite.palette, sprite.flip_x,  sprite.flip_y,
                        sprite.group,    sprite.group_x, sprite.group_y, sprite.group_pc,
                        sprite.blend_weight});
    }
    rust_video::SpriteGroupsHost groups{frame.groups};
    m_stage->present(list, groups, frame.game_frame, frame.flipped);
    sprites.clear();
    sprites.reserve(list.size());
    for (const auto &sprite : list) {
        sprites.push_back({sprite.x,        sprite.y,       sprite.scale_x, sprite.scale_y,
                           sprite.tile,     sprite.palette, sprite.flip_x,  sprite.flip_y,
                           sprite.group,    sprite.group_x, sprite.group_y, sprite.group_pc,
                           sprite.blend_weight});
    }
}

void FlickerHoldStage::reset() { m_stage->reset(); }

void FlickerHoldStage::report(std::ostream &out) const { out << m_stage->report(); }

} // namespace f3rt
