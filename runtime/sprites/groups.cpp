#include "sprites/groups.hpp"
#include "f3rt/machine.hpp"
#include <algorithm>
#include <ostream>
#include <vector>

namespace f3rt {

namespace {
rust::Box<rust_sprites::SpriteGroupTracker> make_rust_tracker(const SpriteGroupTable &table) {
    std::vector<rust_sprites::SpriteGroupWrite> writes;
    writes.reserve(table.writes.size());
    for (const auto &w : table.writes) {
        writes.push_back(
            {w.pc, w.base, w.lane, w.group_class, w.shift, w.x_offset, w.y_offset});
    }
    std::vector<rust_sprites::SpriteGroupReset> resets;
    resets.reserve(table.resets.size());
    for (const auto &r : table.resets) {
        resets.push_back({r.pc, r.base, r.offset});
    }
    return rust_sprites::new_sprite_group_tracker(
        rust::Slice<const rust_sprites::SpriteGroupWrite>(writes.data(), writes.size()),
        rust::Slice<const rust_sprites::SpriteGroupReset>(resets.data(), resets.size()));
}

} // namespace

SpriteGroupTracker::SpriteGroupTracker(Machine &machine, const SpriteGroupTable &table)
    : m_machine(machine), m_inner(make_rust_tracker(table)) {}

std::optional<uint32_t> MachineRam::offset16(uint32_t address) const {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE >= RAM_END - RAM_BASE) return std::nullopt;
    const uint32_t offset = a & RAM_MIRROR_MASK;
    if (offset + 2 > m_ram.size()) return std::nullopt;
    return offset;
}

std::optional<int16_t> MachineRam::read_ram16(uint32_t address) const {
    const auto offset = offset16(address);
    if (!offset) return std::nullopt;
    return int16_t(uint16_t(m_ram[*offset]) << 8 | m_ram[*offset + 1]);
}

MachineRam SpriteGroupTracker::ram() const { return MachineRam(m_machine.ram); }

std::span<const uint8_t> SpriteGroupTracker::main_rom() const { return m_machine.roms.main; }

std::span<const SpriteGroupSlot> SpriteGroupTracker::slots() const {
    m_inner->copy_slots({m_slots_cache.data(), m_slots_cache.size()});
    return m_slots_cache;
}

SpriteGroupSlot SpriteGroupTracker::current_root(uint64_t key) const {
    return m_inner->current_root(key, {m_machine.ram.data(), m_machine.ram.size()});
}

void SpriteGroupTracker::note_write(uint32_t address, unsigned width) {
    rust_sprites::WriteEvent event;
    event.address = address;
    event.width = width;
    event.pc = m_machine.cpu.pc;
    std::copy_n(m_machine.cpu.d, event.d.size(), event.d.begin());
    std::copy_n(m_machine.cpu.a, event.a.size(), event.a.begin());
    event.frame = m_machine.frame;
    m_inner->note_write(event, {m_machine.ram.data(), m_machine.ram.size()});
}

void SpriteGroupTracker::write_summary(std::ostream &out) const {
    out << m_inner->format_summary();
}

void SpriteGroupTracker::write_report(std::ostream &out) const {
    out << m_inner->format_report();
}

}
