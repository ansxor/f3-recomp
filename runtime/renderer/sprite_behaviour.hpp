#pragma once
// Sprite behaviours: per-game patches applied to an emit-unit replay (see
// runtime/sprite_units.hpp). A behaviour reads the unit's bytes as they are at unit
// start and writes field patches into the replay's copy-on-write RAM overlay; the
// game's own native code then re-draws the unit from the patched state. Behaviours
// never touch emulated state.
//
//   // games/<id>/sprites/behaviours.hpp
//   #include "generated_config/sprite_units.hpp"
//   #include "renderer/sprite_behaviour.hpp"
//   namespace f3rt::game_sprites {
//   F3RT_SPRITE_BEHAVIOUR(double_zoom, sprite_units::objects, "Doubles object zoom",
//       [](const Unit<sprite_units::objects> &o, Patch<sprite_units::objects> &p) {
//           p.u16<0x10>(uint16_t(o.u16<0x10>() * 2));
//           return true;   // false: leave this unit unpatched (no splice)
//       });
//   inline constexpr std::array<const SpriteBehaviour *, 1> behaviours{&double_zoom};
//   // required (may be empty): F3RT_FLICKER_SHADOW sources, see below
//   inline constexpr std::array<const FlickerShadow *, 1> flicker_shadows{&shadow};
//   }
#include "f3rt/emit_unit.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace f3rt {

// Type-erased unit memory access. Offsets are relative to the unit address; reads
// outside [0, size) return 0 and patches outside it are ignored (the Unit/Patch
// templates reject such offsets at compile time).
class UnitView {
public:
    using Read = uint32_t (*)(void *context, uint32_t address, unsigned width);
    UnitView(void *context, Read read, uint32_t address, uint32_t size)
        : context_(context), read_(read), address_(address), size_(size) {}
    uint32_t address() const { return address_; }
    uint32_t size() const { return size_; }
    uint32_t read(uint32_t offset, unsigned width) const {
        return offset + width <= size_ ? read_(context_, address_ + offset, width) : 0;
    }
private:
    void *context_;
    Read read_;
    uint32_t address_, size_;
};

class PatchView {
public:
    using Write = void (*)(void *context, uint32_t address, uint32_t value, unsigned width);
    PatchView(void *context, Write write, uint32_t address, uint32_t size)
        : context_(context), write_(write), address_(address), size_(size) {}
    void write(uint32_t offset, unsigned width, uint32_t value) {
        if (offset + width <= size_) write_(context_, address_ + offset, value, width);
    }
private:
    void *context_;
    Write write_;
    uint32_t address_, size_;
};

// Compile-time bounded wrappers: offsets must lie inside the unit's declared size.
template<const EmitUnit &U>
class Unit {
public:
    explicit Unit(const UnitView &view) : view_(view) {}
    uint32_t address() const { return view_.address(); }
    template<uint32_t Off> uint8_t u8() const {
        static_assert(Off + 1 <= U.size, "u8 offset outside the emit unit's declared size");
        return uint8_t(view_.read(Off, 1));
    }
    template<uint32_t Off> uint16_t u16() const {
        static_assert(Off + 2 <= U.size, "u16 offset outside the emit unit's declared size");
        return uint16_t(view_.read(Off, 2));
    }
    template<uint32_t Off> uint32_t u32() const {
        static_assert(Off + 4 <= U.size, "u32 offset outside the emit unit's declared size");
        return view_.read(Off, 4);
    }
private:
    const UnitView &view_;
};

template<const EmitUnit &U>
class Patch {
public:
    explicit Patch(PatchView &view) : view_(view) {}
    template<uint32_t Off> void u8(uint8_t value) {
        static_assert(Off + 1 <= U.size, "u8 offset outside the emit unit's declared size");
        view_.write(Off, 1, value);
    }
    template<uint32_t Off> void u16(uint16_t value) {
        static_assert(Off + 2 <= U.size, "u16 offset outside the emit unit's declared size");
        view_.write(Off, 2, value);
    }
    template<uint32_t Off> void u32(uint32_t value) {
        static_assert(Off + 4 <= U.size, "u32 offset outside the emit unit's declared size");
        view_.write(Off, 4, value);
    }
private:
    PatchView &view_;
};

// `apply` returns true when it patched the unit (the replay is then run and spliced).
struct SpriteBehaviour {
    const char *name;        // identifier with '_' replaced by '-'
    const char *description;
    const EmitUnit *unit;
    bool (*apply)(const UnitView &, PatchView &);
};

namespace detail {
template<size_t N>
struct BehaviourName {
    char text[N]{};
    constexpr BehaviourName(const char (&source)[N]) {
        for (size_t i = 0; i < N; ++i) text[i] = source[i] == '_' ? '-' : source[i];
    }
};
} // namespace detail

#define F3RT_SPRITE_BEHAVIOUR(ident, unit_ref, description, ...) \
    inline constexpr ::f3rt::detail::BehaviourName<sizeof(#ident)> ident##_behaviour_name{#ident}; \
    inline constexpr ::f3rt::SpriteBehaviour ident{ \
        ident##_behaviour_name.text, description, &(unit_ref), \
        [](const ::f3rt::UnitView &view, ::f3rt::PatchView &patch_view) -> bool { \
            const ::f3rt::Unit<unit_ref> unit(view); \
            ::f3rt::Patch<unit_ref> patch(patch_view); \
            return (__VA_ARGS__)(unit, patch); \
        }}

// Flicker shadows: some games fake translucent shadows by emitting the shadow sprite
// entries only on alternate frames (a game-side parity bit). A FlickerShadow names the
// game's own shadow emit path inside a unit so the renderer can (1) identify the shadow
// entries exactly (sprite-RAM writes whose writer PC lies in `emit`, in the real draw and
// in the replay), and (2) keep every shadowed object's shadow logically present: the
// replay forces the game's parity gate so the shadow entries exist on every frame, tagged
// sprite_flag_shadow. The presenter then picks their visibility per *presented* frame
// (renderer/sprite_presentation.hpp, flicker_shadow_visible) instead of per emulated frame.
// Render-only like behaviours: nothing here touches emulated state.
//
//   F3RT_FLICKER_SHADOW(ident, unit, description, emit_first, emit_last,
//                       gate_register, gate_offset, gate_mask, lambda(const Unit<unit> &) -> bool)
//   gate: the RAM byte at <gate_register> + gate_offset; the game emits shadows only
//         while (byte & gate_mask) != 0. The replay sets those bits in its overlay.
//   lambda: true when this unit instance has a shadow (read at unit start).
struct FlickerShadow {
    const char *name;        // identifier with '_' replaced by '-'
    const char *description;
    const EmitUnit *unit;
    PcRange emit;            // inclusive PCs of the shadow entry writers
    UnitRegister gate_register;
    int32_t gate_offset;
    uint8_t gate_mask;
    bool (*applies)(const UnitView &);
};

#define F3RT_FLICKER_SHADOW(ident, unit_ref, description, emit_first, emit_last, gate_register, gate_offset, \
                            gate_mask, ...) \
    inline constexpr ::f3rt::detail::BehaviourName<sizeof(#ident)> ident##_flicker_name{#ident}; \
    inline constexpr ::f3rt::FlickerShadow ident{ \
        ident##_flicker_name.text, description, &(unit_ref), ::f3rt::PcRange{emit_first, emit_last}, \
        gate_register, gate_offset, gate_mask, \
        [](const ::f3rt::UnitView &view) -> bool { \
            const ::f3rt::Unit<unit_ref> unit(view); \
            return (__VA_ARGS__)(unit); \
        }}

// Behaviours compiled into this executable (games/<id>/sprites/behaviours.hpp, via
// F3RT_SPRITE_BEHAVIOURS_HEADER); empty when the game has none.
std::span<const SpriteBehaviour *const> registered_sprite_behaviours();
// Flicker shadow sources of the game (`game_sprites::flicker_shadows`, required in the same header); empty when none.
std::span<const FlickerShadow *const> registered_flicker_shadows();

} // namespace f3rt
