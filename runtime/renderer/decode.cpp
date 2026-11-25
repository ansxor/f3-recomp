// license:BSD-3-Clause
#include "renderer/decode.hpp"
#include "f3rt_video_cxx/video.h"
#include <vector>

namespace f3rt {

size_t decode_sprite_list(const uint8_t *spriteram, int visible_y, int visible_height,
                          std::span<DecodedSpriteEntry> out, SpriteRamState &state, bool apply_flip,
                          const SpritePresentation *presentation) {
    auto presented = rust_video::new_sprite_presentation();
    if (presentation) {
        for (const auto &group : presentation->groups)
            presented->push_group(group.key, group.x, group.y, group.pc);
    }

    std::vector<rust_video::DecodedSpriteEntry> decoded(out.size());
    rust_video::SpriteRamState ram{state.flipscreen, state.bank, state.trails, state.extra_planes,
                                   state.pen_mask};
    const auto entries =
        rust::Slice<rust_video::DecodedSpriteEntry>(decoded.data(), decoded.size());
    const size_t count = rust_video::decode_sprites(
        rust::Slice<const uint8_t>(spriteram, 0x10000), visible_y, visible_height, entries, ram,
        apply_flip, *presented);
    for (size_t i = 0; i < count && i < out.size(); ++i) {
        const auto &src = decoded[i];
        out[i] = {src.x,        src.y,     src.scale_x, src.scale_y, src.tile, src.color,
                  src.flip_x,   src.flip_y, src.pri,    src.group,   src.group_x, src.group_y,
                  src.group_pc};
    }
    state.flipscreen = ram.flipscreen;
    state.bank = ram.bank;
    state.trails = ram.trails;
    state.extra_planes = ram.extra_planes;
    state.pen_mask = ram.pen_mask;
    return count;
}

} // namespace f3rt
