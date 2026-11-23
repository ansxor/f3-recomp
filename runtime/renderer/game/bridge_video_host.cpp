#include "f3rt/bridge_video_host.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt_video_cxx/video.h"
#include "f3rt/renderer/game/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include "sprites/groups.hpp"
#include <algorithm>
#include <bit>
#include <optional>
#include <sstream>

namespace f3rt::rust_video {

bool EnhancementsHost::empty() const { return !enhancements || enhancements->empty(); }

size_t EnhancementsHost::present(rust::Slice<const SceneSprite> latched, bool flipped,
                                 rust::Slice<SceneSprite> out) {
    if (!enhancements) return 0;
    std::optional<SpriteFrame> frame;
    if (tracker) frame = SpriteFrame::read(*tracker, flipped);
    enhancements->present_from_rust({latched.data(), latched.size()}, frame);
    const auto presented = enhancements->sprites();
    const size_t count = std::min(presented.size(), out.size());
    std::transform(presented.begin(), presented.begin() + count, out.begin(),
                   [](const f3rt::SceneSprite &sprite) { return std::bit_cast<SceneSprite>(sprite); });
    return count;
}

void EnhancementsHost::reset() {
    if (enhancements) enhancements->reset();
}

rust::String EnhancementsHost::report() const {
    if (!enhancements) return {};
    std::ostringstream text;
    enhancements->report(text);
    return rust::String(text.str());
}

void ProbeHost::enter(uint8_t stage) {
    if (probe) probe->enter(VideoStage(stage));
}

SpriteRoot SpriteGroupsHost::current_root(uint64_t key) const {
    if (!tracker) return {};
    const SpriteGroupSlot root = tracker->current_root(key);
    return {root.key, root.x, root.y, root.pc};
}

} // namespace f3rt::rust_video
