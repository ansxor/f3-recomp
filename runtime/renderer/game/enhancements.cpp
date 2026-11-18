#include "f3rt/renderer/game/enhancements.hpp"
#include "generated_config/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include "sprites/groups.hpp"
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
    if (frame)
        for (const auto &stage : m_stages) stage->present(m_presented, *frame);
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
