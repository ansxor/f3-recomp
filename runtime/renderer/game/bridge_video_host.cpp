#include "f3rt/bridge_video_host.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/renderer/game/enhancements.hpp"
#include "renderer/game/scene.hpp"
#include "sprites/groups.hpp"
#include <algorithm>
#include <cstring>
#include <optional>
#include <sstream>

namespace f3rt::rust_video {
namespace {
std::span<const SceneSprite> sprites_from_bytes(rust::Slice<const uint8_t> bytes) {
    return {reinterpret_cast<const SceneSprite *>(bytes.data()),
            bytes.size() / sizeof(SceneSprite)};
}
} // namespace

bool EnhancementsHost::empty() const { return !enhancements || enhancements->empty(); }

size_t EnhancementsHost::present(rust::Slice<const uint8_t> latched, bool flipped,
                                 rust::Slice<uint8_t> out) {
    if (!enhancements) return 0;
    const auto input = sprites_from_bytes(latched);
    std::optional<SpriteFrame> frame;
    if (tracker) frame = SpriteFrame::read(*tracker, flipped);
    enhancements->present(input, frame);
    const auto presented = enhancements->sprites();
    const size_t count =
        std::min(presented.size(), out.size() / sizeof(SceneSprite));
    if (count)
        std::memcpy(out.data(), presented.data(), count * sizeof(SceneSprite));
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

uint64_t SpriteGroupsHost::current_root(uint64_t key, int32_t &x, int32_t &y,
                                        uint32_t &pc) const {
    if (!tracker) return 0;
    const SpriteGroupSlot root = tracker->current_root(key);
    x = root.x;
    y = root.y;
    pc = root.pc;
    return root.key;
}

} // namespace f3rt::rust_video
