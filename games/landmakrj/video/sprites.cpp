// Land Maker sprite-layer VRAM decode.
//
// The sprite display list at 0x600000 (0x10000 bytes, 1024 entries of 16 bytes)
// is walked with block chaining, the list jump word, command/scaled/scroll words
// and the sprite-bank select. This is the same hardware list Video::get_sprite_info
// reads, decoded here at VBSTART instead of from hook-time observations.
#include "game_sprites.hpp"
#ifdef F3RT_VIDEO_WRITE_LOG
#include "game_video_log.hpp"
#endif
#include "video_decode.hpp"
#include <algorithm>

namespace f3rt {

void GameSprites::decode(const VideoRam &vram) {
    SpriteRamState state;
    state.flipscreen = reg_flipped_;
    state.bank = reg_bank_;
    state.trails = reg_trails_;
    state.pen_mask = reg_pen_mask_;
    state.extra_planes = uint8_t(reg_pen_mask_ >> 4);

    std::array<DecodedSpriteEntry, kMaxSprites> decoded;
    const size_t count = decode_sprite_list(vram.graphics.data(), 24, 232, decoded, state, false);

    for (size_t i = 0; i < count; ++i) {
        auto &dst = staging_sprites_[i];
        dst.x = decoded[i].x;
        dst.y = decoded[i].y;
        dst.scale_x = uint16_t(decoded[i].scale_x);
        dst.scale_y = uint16_t(decoded[i].scale_y);
        dst.tile = decoded[i].tile;
        dst.palette = decoded[i].color;
        dst.flip_x = decoded[i].flip_x;
        dst.flip_y = decoded[i].flip_y;
    }
    // The decoded list is this frame's submission; GameVideo::latch_sprites()
    // applies the scanout origin and the one-frame lag.
    std::copy_n(staging_sprites_.begin(), count, submitted_sprites_.begin());
    submitted_count_ = count;
    staging_count_ = 0;

    reg_flipped_ = state.flipscreen;
    reg_pen_mask_ = state.pen_mask;
    reg_trails_ = state.trails;
    reg_bank_ = state.bank;
}

#ifdef F3RT_VIDEO_WRITE_LOG
namespace {
// Store PCs Land Maker uses to build the sprite display list. Ranges mirror the
// retired hook coverage: init_sprites, clear_spriteram, sprite_scroll,
// sprite_command, list terminator and the single/grid/scaled/object compilers.
bool is_covered_write(uint32_t pc) {
    if (pc >= 0x41d0 && pc <= 0x4380) return true;
    if (pc >= 0x43b0 && pc <= 0x43de) return true; // sprite_scroll
    if (pc >= 0x43e0 && pc <= 0x43fe) return true; // sprite_command
    if (pc >= 0x4422 && pc <= 0x447e) return true; // list terminator
    if (pc >= 0x4688 && pc <= 0x46be) return true; // single sprite compiler
    if (pc >= 0x46c0 && pc <= 0x480a) return true; // grid sprite compiler
    if (pc >= 0x480c && pc <= 0x4a36) return true; // scaled grid compiler
    if (pc >= 0xa8f38 && pc <= 0xa8f82) return true; // obj_grid
    if (pc >= 0xa8f84 && pc <= 0xa8fce) return true; // obj_3tile
    if (pc >= 0xa9036 && pc <= 0xa9076) return true; // obj_grid flip_x
    if (pc >= 0xa90f4 && pc <= 0xa913a) return true; // obj_4tile
    if (pc >= 0xa913c && pc <= 0xa93a2) return true; // obj_scaled
    return false;
}
} // namespace

void GameSprites::observe_write(uint32_t pc, uint32_t address, uint64_t frame) {
    if (address < 0x600000 || address >= 0x610000) return;
    if (is_covered_write(pc)) return;
    log_unknown_video_write("sprites", pc, address, frame);
}
#endif

} // namespace f3rt
