#pragma once
// Command War sprite behaviours (see runtime/renderer/sprite_behaviour.hpp).
#include "generated_config/sprite_units.hpp"
#include "renderer/sprite_behaviour.hpp"

#include <array>

namespace f3rt::game_sprites {

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
F3RT_SPRITE_BEHAVIOUR(full_detail, sprite_units::objects,
    "Draws objects at the full-detail mip level instead of the distance-selected level",
    [](const Unit<sprite_units::objects> &object, Patch<sprite_units::objects> &patch) {
        if (!(object.u8<1>() & 0x0c) || !object.u16<0x2a>()) return false;
        patch.u8<1>(uint8_t(object.u8<1>() & ~0x0c));
        patch.u16<2>(object.u16<0x2a>());
        return true;
    });

inline constexpr std::array<const SpriteBehaviour *, 1> behaviours{&full_detail};

// Shadows under objects are drawn translucent by flicker: the game emits the shadow entries only
// on alternate frames of its own frame counter.
//
//   0x680a  addq.w #1,-$7cd8(a5)    main loop, once per game frame (a5 = 0x410000, so the counter
//                                   word is 0x408328 and its low byte, bit 0 = parity, 0x408329)
//   0x9b32  object compile: `btst #7,$1(a6)` (object has a shadow) -> else straight to 0x9d32;
//   0x9b42  `btst #0,-$7cd7(a5)` (counter parity) -> else straight to 0x9d32 (no shadow this frame)
//   0x9b4c..0x9d30  shadow entries from $40 (descriptor), $42-$44 (offsets), flip/anchor bits;
//                   the shadow's own tiles are emitted first, 0x9d32.. then compiles the object body.
// So the shadow exists on every other game frame (counter bit 0 set) and nothing but that parity
// gates it. The source forces the parity bit in the replay (shadows exist every frame, tagged) and
// tags every entry written by PCs 0x9b32..0x9d31 (the subroutines called from the span at
// 0xa30e/0xa31e only adjust a0/a2 and write no sprite RAM).
F3RT_FLICKER_SHADOW(object_shadow, sprite_units::objects,
    "Object shadows: byte1 bit 7 of the record, emitted by the game only on odd counter parity",
    0x9b32, 0x9d31, UnitRegister::A5, -0x7cd7, 0x01,
    [](const Unit<sprite_units::objects> &object) { return (object.u8<1>() & 0x80) != 0; });

inline constexpr std::array<const FlickerShadow *, 1> flicker_shadows{&object_shadow};

} // namespace f3rt::game_sprites
