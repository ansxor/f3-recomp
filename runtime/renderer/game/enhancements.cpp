#include "f3rt/renderer/game/enhancements.hpp"
#include "generated_config/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include "sprites/groups.hpp"
#include "f3rt_video_cxx/video.h"
#include <bit>
#include <ranges>
#include <utility>

namespace f3rt {

std::optional<SpriteFrame> SpriteFrame::read(const SpriteGroupTracker &groups, bool flipped) {
    const auto counter = groups.ram().read_ram16(game_config::game_frame_counter);
    if (!counter) return std::nullopt;
    return SpriteFrame{&groups, uint16_t(*counter), flipped};
}

RenderEnhancements::RenderEnhancements() = default;

RenderEnhancements::RenderEnhancements(std::vector<std::unique_ptr<SpriteStage>> stages)
    : m_stages(std::move(stages)) {}

RenderEnhancements::RenderEnhancements(RenderEnhancements &&) noexcept = default;
RenderEnhancements &RenderEnhancements::operator=(RenderEnhancements &&) noexcept = default;
RenderEnhancements::~RenderEnhancements() = default;

void RenderEnhancements::present(std::span<const SceneSprite> latched,
                                 std::optional<SpriteFrame> frame) {
    if (!frame) reset();
    m_presented.assign(latched.begin(), latched.end());
    if (frame) apply_stages(*frame);
}

void RenderEnhancements::present_from_rust(std::span<const rust_video::SceneSprite> latched,
                                           std::optional<SpriteFrame> frame) {
    if (!frame) reset();
    auto sprites = latched | std::views::transform([](const rust_video::SceneSprite &sprite) {
        return std::bit_cast<SceneSprite>(sprite);
    });
    m_presented.assign(sprites.begin(), sprites.end());
    if (frame) apply_stages(*frame);
}

void RenderEnhancements::apply_stages(const SpriteFrame &frame) {
    for (const auto &stage : m_stages) stage->present(m_presented, frame);
}

std::span<const SceneSprite> RenderEnhancements::sprites() const { return m_presented; }

void RenderEnhancements::reset() {
    m_presented.clear();
    for (const auto &stage : m_stages) stage->reset();
}

void RenderEnhancements::report(std::ostream &out) const {
    for (const auto &stage : m_stages) stage->report(out);
}

}
