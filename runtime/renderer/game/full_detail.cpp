#include "renderer/game/full_detail.hpp"
#include "f3rt/bridge_video_host.hpp"
#include "sprites/groups.hpp"
#include <ostream>

namespace f3rt {

FullDetailStage::FullDetailStage(int visible_y, int visible_height)
    : m_stage(rust_video::new_commandw_full_detail(int32_t(visible_y), int32_t(visible_height))) {}

namespace {
template<class Dest>
Dest sprite_as(const auto &sprite) {
    return {sprite.x, sprite.y, sprite.scale_x, sprite.scale_y, sprite.tile, sprite.palette,
            sprite.flip_x, sprite.flip_y, sprite.group, sprite.group_x, sprite.group_y,
            sprite.group_pc, sprite.blend_weight};
}
}

void FullDetailStage::present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) {
    m_bridge_sprites.clear();
    m_bridge_sprites.reserve(sprites.size());
    for (const auto &sprite : sprites) m_bridge_sprites.push_back(sprite_as<rust_video::SceneSprite>(sprite));

    const rust_video::SpriteGroupsHost groups{frame.groups};
    const auto rom = frame.groups ? frame.groups->main_rom() : std::span<const uint8_t>{};
    m_stage->present(m_bridge_sprites, groups, {rom.data(), rom.size()}, frame.flipped);

    sprites.clear();
    sprites.reserve(m_bridge_sprites.size());
    for (const auto &sprite : m_bridge_sprites) sprites.push_back(sprite_as<SceneSprite>(sprite));
}

void FullDetailStage::reset() { m_stage->reset(); }

void FullDetailStage::report(std::ostream &out) const { out << m_stage->report(); }

}
