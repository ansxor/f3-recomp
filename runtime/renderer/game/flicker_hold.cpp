#include "renderer/game/flicker_hold.hpp"
#include "sprites/groups.hpp"
#include <algorithm>
#include <ostream>
#include <unordered_set>

namespace f3rt {

FlickerHoldStage::FlickerHoldStage(std::span<const game_config::FlickerHold> holds) {
    for (const auto &hold : holds) m_holds.push_back({hold, {}, {}});
}

std::optional<FlickerFrame> FlickerFrame::read(const SpriteGroupTracker &groups, bool flipped) {
    const auto counter = groups.ram().read_ram16(game_config::game_frame_counter);
    if (!counter) return std::nullopt;
    return FlickerFrame{&groups, uint16_t(*counter), flipped};
}

void FlickerHoldStage::present(std::span<const SceneSprite> latched,
                               std::optional<FlickerFrame> frame) {
    if (!frame || frame->flipped != m_flipped) reset();
    m_presented.assign(latched.begin(), latched.end());
    if (!frame) return;
    const auto &[tracker, game_frame, flipped] = *frame;
    m_flipped = flipped;
    auto &sprites = m_presented;

    for (auto &hold : m_holds) {
        const auto &config = hold.config;
        std::unordered_map<uint64_t, size_t> anchors;
        std::unordered_set<uint64_t> present;
        for (size_t index = 0; index < sprites.size(); ++index) {
            auto &sprite = sprites[index];
            if (!sprite.group) continue;
            const unsigned cls = group_key_class(sprite.group);
            if (cls == config.anchor_class) anchors.try_emplace(sprite.group, index);
            if (cls != config.group_class) continue;
            auto &held = hold.groups[sprite.group];
            if (present.insert(sprite.group).second) {
                held.game_frame = game_frame;
                held.sprites.clear();
            }
            sprite.blend_weight = 1.0f / config.period;
            held.sprites.push_back(sprite);
        }

        const bool self = config.follow == game_config::FlickerHold::Follow::Self;
        const auto anchor_key_for = [&](uint64_t key) {
            return pack_group_key(group_key_base(key), config.anchor_class,
                                  uint16_t(key >> 16), uint16_t(key));
        };
        if (!self) {
            for (const uint64_t key : present) {
                auto &held = hold.groups.at(key);
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
        for (auto it = hold.groups.begin(); it != hold.groups.end();) {
            const auto &[key, held] = *it;
            if (present.contains(key)) {
                ++it;
                continue;
            }
            const auto anchor = anchors.find(anchor_key_for(key));
            if (anchor == anchors.end()) ++hold.stats.anchor_missing;
            if (anchor == anchors.end() || (!self && !held.has_anchor_root) ||
                uint16_t(game_frame - held.game_frame) >= config.period ||
                sprites.size() + extra + held.sprites.size() > MAX_PRESENTED_SPRITES) {
                ++hold.stats.drops;
                it = hold.groups.erase(it);
                continue;
            }
            insertions.emplace_back(anchor->second, key);
            extra += held.sprites.size();
            ++it;
        }
        if (insertions.empty()) continue;

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
                ++hold.stats.drops;
                hold.groups.erase(key);
                continue;
            }
            const auto &held = hold.groups.at(key);
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
            ++hold.stats.held_frames;
        }
    }
}

void FlickerHoldStage::reset() {
    m_presented.clear();
    for (auto &hold : m_holds) hold.groups.clear();
}

void FlickerHoldStage::report(std::ostream &out) const {
    for (const auto &hold : m_holds)
        out << "flicker hold " << hold.config.name << ": held_frames=" << hold.stats.held_frames
            << " drops=" << hold.stats.drops << " anchor_missing=" << hold.stats.anchor_missing << '\n';
}

}
