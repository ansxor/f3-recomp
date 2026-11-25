#pragma once
#include "rust/cxx.h"
#include <cstddef>
#include <cstdint>

namespace f3rt {
class RenderEnhancements;
class SpriteGroupTracker;
class VideoProbe;
}

namespace f3rt::rust_video {

struct SceneSprite;
struct SpriteRoot;
struct FullDetailRecord;

struct EnhancementsHost {
    RenderEnhancements *enhancements = nullptr;
    const SpriteGroupTracker *tracker = nullptr;

    bool empty() const;
    size_t present(rust::Slice<const SceneSprite> latched, bool flipped,
                   rust::Slice<SceneSprite> out);
    void reset();
    rust::String report() const;
};

struct ProbeHost {
    VideoProbe *probe = nullptr;
    void enter(uint8_t stage);
};

struct SpriteGroupsHost {
    const SpriteGroupTracker *tracker = nullptr;
    SpriteRoot current_root(uint64_t key) const;
    FullDetailRecord full_detail_record(uint64_t key, int32_t group_x, int32_t group_y) const;
};

} // namespace f3rt::rust_video
