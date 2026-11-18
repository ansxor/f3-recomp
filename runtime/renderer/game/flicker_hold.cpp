#include "renderer/game/flicker_hold.hpp"
#include "sprites/groups.hpp"
#include <algorithm>
#include <ostream>
#include <unordered_set>

namespace f3rt {

void FlickerHoldStage::present(std::vector<SceneSprite> &sprites, const SpriteFrame &frame) {
    const auto &[tracker, game_frame, flipped] = frame;
    if (flipped != m_flipped) reset();
    m_flipped = flipped;

    std::unordered_map<uint64_t, size_t> anchors;
    std::unordered_set<uint64_t> present;
    for (size_t index = 0; index < sprites.size(); ++index) {
        auto &sprite = sprites[index];
        if (!sprite.group) continue;
        const unsigned cls = group_key_class(sprite.group);
        if (cls == m_config.anchor_class) anchors.try_emplace(sprite.group, index);
        if (cls != m_config.group_class) continue;
        auto &held = m_groups[sprite.group];
        if (present.insert(sprite.group).second) {
            held.game_frame = game_frame;
            held.sprites.clear();
        }
        sprite.blend_weight = 1.0f / m_config.period;
        held.sprites.push_back(sprite);
    }

    const bool self = m_config.follow == game_config::FlickerHold::Follow::Self;
    const auto anchor_key_for = [&](uint64_t key) {
        return pack_group_key(group_key_base(key), m_config.anchor_class,
                              uint16_t(key >> 16), uint16_t(key));
    };
    if (!self) {
        for (const uint64_t key : present) {
            auto &held = m_groups.at(key);
            const auto anchor = anchors.find(anchor_key_for(key));
            held.has_anchor_root = anchor != anchors.end();
            if (held.has_anchor_root) {
                held.anchor_x = sprites[anchor->second].group_x;
                held.anchor_y = sprites[anchor->second].group_y;
            }
        }
    }

    std::vector<std::pair<size_t, uint64_t>> insertions;
    size_t extra = 0;
    for (auto it = m_groups.begin(); it != m_groups.end();) {
        const auto &[key, held] = *it;
        if (present.contains(key)) {
            ++it;
            continue;
        }
        const auto anchor = anchors.find(anchor_key_for(key));
        if (anchor == anchors.end()) ++m_stats.anchor_missing;
        if (anchor == anchors.end() || (!self && !held.has_anchor_root) ||
            uint16_t(game_frame - held.game_frame) >= m_config.period ||
            sprites.size() + extra + held.sprites.size() > MAX_PRESENTED_SPRITES) {
            ++m_stats.drops;
            it = m_groups.erase(it);
            continue;
        }
        insertions.emplace_back(anchor->second, key);
        extra += held.sprites.size();
        ++it;
    }
    if (insertions.empty()) return;

    std::ranges::sort(insertions);
    std::vector<SceneSprite> output;
    output.reserve(sprites.size() + extra);
    size_t previous = 0;
    bool drew_held = false;
    for (const auto &[index, key] : insertions) {
        const auto &anchor = sprites[index];
        const auto current_root =
            self ? tracker->current_root(key)
                 : SpriteGroupSlot{anchor.group, anchor.group_x, anchor.group_y};
        if (!current_root.key) {
            ++m_stats.drops;
            m_groups.erase(key);
            continue;
        }
        const auto &held = m_groups.at(key);
        const auto &origin = held.sprites.front();
        const int32_t x = self ? origin.group_x : held.anchor_x;
        const int32_t y = self ? origin.group_y : held.anchor_y;
        const int32_t dx = current_root.x - x;
        const int32_t dy = current_root.y - y;
        output.insert(output.end(), sprites.begin() + previous, sprites.begin() + index);
        previous = index;
        for (auto sprite : held.sprites) {
            sprite.x += flipped ? -dx : dx;
            sprite.y += flipped ? -dy : dy;
            sprite.group_x += dx;
            sprite.group_y += dy;
            output.push_back(sprite);
        }
        drew_held = true;
    }
    if (drew_held) {
        output.insert(output.end(), sprites.begin() + previous, sprites.end());
        sprites.swap(output);
        ++m_stats.held_frames;
    }
}

void FlickerHoldStage::reset() { m_groups.clear(); }

void FlickerHoldStage::report(std::ostream &out) const {
    out << "flicker hold " << m_config.name << ": held_frames=" << m_stats.held_frames
        << " drops=" << m_stats.drops << " anchor_missing=" << m_stats.anchor_missing << '\n';
}

}
