#pragma once
// Sprite behaviours: per-game patches applied to an emit-unit replay (see
// runtime/sprites/replay.hpp). A behaviour reads the unit's bytes as they are at unit
// start and writes field patches into the replay's copy-on-write RAM overlay; the
// game's own native code then re-draws the unit from the patched state. Behaviours
// never touch emulated state, and emit units exist only as behaviour anchors: grouping and
// identity come from the generated sprite-group table.
//
//   // games/<id>/sprites/behaviours.hpp
//   #include "generated_config/sprite_units.hpp"
//   #include "sprites/behaviour.hpp"
//   namespace f3rt::game_sprites {
//   F3RT_SPRITE_BEHAVIOUR(DOUBLE_ZOOM, sprite_units::objects, "Doubles object zoom",
//       [](const Unit<sprite_units::objects> &o, Patch<sprite_units::objects> &p) {
//           p.u16<0x10>(uint16_t(o.u16<0x10>() * 2));
//           return true;   // false: leave this unit unpatched (no splice)
//       });
//   inline constexpr std::array<const SpriteBehaviour *, 1> BEHAVIOURS{&DOUBLE_ZOOM};
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
        : m_context(context), m_read(read), m_address(address), m_size(size) {}
    uint32_t read(uint32_t offset, unsigned width) const {
        return offset + width <= m_size ? m_read(m_context, m_address + offset, width) : 0;
    }

private:
    void *m_context;
    Read m_read;
    uint32_t m_address, m_size;
};

class PatchView {
public:
    using Write = void (*)(void *context, uint32_t address, uint32_t value, unsigned width);
    PatchView(void *context, Write write, uint32_t address, uint32_t size)
        : m_context(context), m_write(write), m_address(address), m_size(size) {}
    void write(uint32_t offset, unsigned width, uint32_t value) {
        if (offset + width <= m_size) m_write(m_context, m_address + offset, value, width);
    }

private:
    void *m_context;
    Write m_write;
    uint32_t m_address, m_size;
};

// Compile-time bounded wrappers: offsets must lie inside the unit's declared size.
template <const EmitUnit &U> class Unit {
public:
    explicit Unit(const UnitView &view) : m_view(view) {}
    template <uint32_t Off> uint8_t u8() const {
        static_assert(Off + 1 <= U.size, "u8 offset outside the emit unit's declared size");
        return uint8_t(m_view.read(Off, 1));
    }
    template <uint32_t Off> uint16_t u16() const {
        static_assert(Off + 2 <= U.size, "u16 offset outside the emit unit's declared size");
        return uint16_t(m_view.read(Off, 2));
    }
    template <uint32_t Off> uint32_t u32() const {
        static_assert(Off + 4 <= U.size, "u32 offset outside the emit unit's declared size");
        return m_view.read(Off, 4);
    }

private:
    const UnitView &m_view;
};

template <const EmitUnit &U> class Patch {
public:
    explicit Patch(PatchView &view) : m_view(view) {}
    template <uint32_t Off> void u8(uint8_t value) {
        static_assert(Off + 1 <= U.size, "u8 offset outside the emit unit's declared size");
        m_view.write(Off, 1, value);
    }
    template <uint32_t Off> void u16(uint16_t value) {
        static_assert(Off + 2 <= U.size, "u16 offset outside the emit unit's declared size");
        m_view.write(Off, 2, value);
    }
    template <uint32_t Off> void u32(uint32_t value) {
        static_assert(Off + 4 <= U.size, "u32 offset outside the emit unit's declared size");
        m_view.write(Off, 4, value);
    }

private:
    PatchView &m_view;
};

// `apply` returns true when it patched the unit (the replay is then run and spliced).
struct SpriteBehaviour {
    const char *name; // identifier lowercased, '_' replaced by '-'
    const char *description;
    const EmitUnit *unit;
    bool (*apply)(const UnitView &, PatchView &);
};

namespace detail {
template <size_t N> struct BehaviourName {
    char text[N]{};
    constexpr BehaviourName(const char (&source)[N]) {
        for (size_t i = 0; i < N; ++i)
            text[i] = source[i] == '_'
                          ? '-'
                          : (source[i] >= 'A' && source[i] <= 'Z' ? char(source[i] - 'A' + 'a')
                                                                  : source[i]);
    }
};
} // namespace detail

#define F3RT_SPRITE_BEHAVIOUR(ident, unit_ref, description, ...)                                   \
    inline constexpr ::f3rt::detail::BehaviourName<sizeof(#ident)> ident##_behaviour_name{#ident}; \
    inline constexpr ::f3rt::SpriteBehaviour ident {                                               \
        ident##_behaviour_name.text, description, &(unit_ref),                                     \
            [](const ::f3rt::UnitView &view, ::f3rt::PatchView &patch_view) -> bool {              \
                const ::f3rt::Unit<unit_ref> unit(view);                                           \
                ::f3rt::Patch<unit_ref> patch(patch_view);                                         \
                return (__VA_ARGS__)(unit, patch);                                                 \
            }                                                                                      \
    }

// Behaviours compiled into this executable (games/<id>/sprites/behaviours.hpp, via
// F3RT_SPRITE_BEHAVIOURS_HEADER); empty when the game has none.
std::span<const SpriteBehaviour *const> registered_sprite_behaviours();

} // namespace f3rt
