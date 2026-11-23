#pragma once
// Host-side adaptors the Rust renderer bridge (`f3rt::rust_video`) calls back into.
// Keep this header free of bridge-generated types: the generated bridge header includes it
// before declaring those structs, so only primitives, `rust::Slice` and f3rt renderer types
// may appear here.
#include "rust/cxx.h"
#include <cstddef>
#include <cstdint>

namespace f3rt {
class RenderEnhancements;
class SpriteGroupTracker;
class VideoProbe;
}

namespace f3rt::rust_video {

// `RenderEnhancements` plus the sprite-group tracker a frame's stages read.
struct EnhancementsHost {
    RenderEnhancements *enhancements = nullptr;
    const SpriteGroupTracker *tracker = nullptr;

    bool empty() const;
    // Decode `latched` (C++ `SceneSprite` array bytes), run the stages and write the presented
    // list back into `out`. Returns the number of sprites written.
    size_t present(rust::Slice<const uint8_t> latched, bool flipped, rust::Slice<uint8_t> out);
    void reset();
    rust::String report() const;
};

struct ProbeHost {
    VideoProbe *probe = nullptr;
    void enter(uint8_t stage);
};

struct SpriteGroupsHost {
    const SpriteGroupTracker *tracker = nullptr;
    // Returns the root key (0 = none) and its decoded position; mirrors `current_root`.
    uint64_t current_root(uint64_t key, int32_t &x, int32_t &y, uint32_t &pc) const;
};

} // namespace f3rt::rust_video
