#pragma once
#include "f3rt/sprite_group.hpp"
#include "f3rt_sprites_cxx/sprites.h"
#include "sprites/presentation.hpp"
#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>

namespace f3rt {
class Machine;

class MachineRam {
public:
    explicit MachineRam(std::span<const uint8_t> ram) : m_ram(ram) {}
    std::optional<uint32_t> offset16(uint32_t address) const;
    std::optional<int16_t> read_ram16(uint32_t address) const;

private:
    std::span<const uint8_t> m_ram;
};

class SpriteGroupTracker {
public:
    SpriteGroupTracker(Machine &machine, const SpriteGroupTable &table);
    SpriteGroupTracker(const SpriteGroupTracker &) = delete;
    SpriteGroupTracker &operator=(const SpriteGroupTracker &) = delete;

    bool groups_active() const { return m_inner->groups_active(); }
    std::span<const SpriteGroupSlot> slots() const;
    MachineRam ram() const;
    std::span<const uint8_t> main_rom() const;

    void enable_commandw_full_detail() { m_inner->enable_commandw_full_detail(); }
    rust_sprites::FullDetailRecord full_detail_record(uint64_t key, int32_t group_x,
                                                      int32_t group_y) const {
        return m_inner->full_detail_record(key, group_x, group_y);
    }

    SpriteGroupSlot current_root(uint64_t key) const;
    void note_write(uint32_t address, unsigned width);
    void count_ambiguity() { m_inner->count_ambiguity(); }
    void frame_end() { m_inner->frame_end(); }
    void reset() { m_inner->reset(); }

    void write_summary(std::ostream &out) const;
    void write_report(std::ostream &out) const;

private:
    Machine &m_machine;
    rust::Box<rust_sprites::SpriteGroupTracker> m_inner;
    mutable std::array<SpriteGroupSlot, SPRITE_SLOTS> m_slots_cache{};
};

}
