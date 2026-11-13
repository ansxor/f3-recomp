#pragma once
// Command War sprite behaviours (see runtime/sprites/behaviour.hpp).
#include "generated_config/sprite_units.hpp"
#include "sprites/behaviour.hpp"

#include <array>

namespace f3rt::game_sprites {
// Object record field offsets
constexpr size_t COMMANDW_OBJ_FLAGS_OFFSET = 1;
constexpr size_t COMMANDW_OBJ_DRAWN_DESCRIPTOR_OFFSET = 2;
constexpr size_t COMMANDW_OBJ_FULL_DETAIL_DESCRIPTOR_OFFSET = 0x2a;

// Draw every object at its full-detail (LoD 0) mip level.
//
// Object records (a6 = 0x410000 + k * 0x80, the `objects` unit, span 0x9a3c..0x9a6c):
//   byte1 bits 2-3  LoD class k (0 = full detail), set by the selector at 0x2fed6
//   u16 $2          descriptor of the level currently drawn (via table 0x5a02c in 0x9d32)
//   u16 $2a         descriptor of the full-detail level (0 = the object has no mip chain)
// 0x2fed6 picks k from the projected zoom ($5 thresholds 0x80/0xc0/0xe0) and stores
// $2 = $2a + k * stride (verified on live objects, e.g. base 0x0485: L0..L3 grids
// 10x12, 5x6, 3x3, 2x2 tiles). The span replays 0xa1c8/0xa3ee (projection, which
// recomputes zoom $4/$5), the LoD zoom rescale 0xa334 ($4 = ($4 - T[k]) << k with
// T = {0, 0x80, 0xc0, 0xe0} at 0xa372, an identity for k = 0) and the compile at
// 0x9b32/0x9d32. Patching class to 0 and $2 to $2a therefore draws the L0 tiles at
// the zoom the unscaled projection yields, i.e. the on-screen size the coarse level
// was scaled up to cover.
F3RT_SPRITE_BEHAVIOUR(
    FULL_DETAIL, sprite_units::objects,
    "Draws objects at the full-detail mip level instead of the distance-selected level",
    [](const Unit<sprite_units::objects> &object, Patch<sprite_units::objects> &patch) {
        if (!(object.u8<COMMANDW_OBJ_FLAGS_OFFSET>() & 0x0c) ||
            !object.u16<COMMANDW_OBJ_FULL_DETAIL_DESCRIPTOR_OFFSET>())
            return false;
        patch.u8<COMMANDW_OBJ_FLAGS_OFFSET>(
            uint8_t(object.u8<COMMANDW_OBJ_FLAGS_OFFSET>() & ~0x0c));
        patch.u16<COMMANDW_OBJ_DRAWN_DESCRIPTOR_OFFSET>(
            object.u16<COMMANDW_OBJ_FULL_DETAIL_DESCRIPTOR_OFFSET>());
        return true;
    });

inline constexpr std::array<const SpriteBehaviour *, 1> BEHAVIOURS{&FULL_DETAIL};


} // namespace f3rt::game_sprites
