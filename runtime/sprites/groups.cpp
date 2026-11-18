#include "sprites/groups.hpp"
#include "f3rt/machine.hpp"
#include <algorithm>

namespace f3rt {

SpriteGroupTracker::SpriteGroupTracker(Machine &machine, const SpriteGroupTable &table)
    : m_machine(machine) {
    compile_table(table);
}

void SpriteGroupTracker::compile_table(const SpriteGroupTable &table) {
    m_writes.reserve(table.writes.size());
    for (const SpriteGroupWrite &write : table.writes) {
        if (write.group_class == 0) continue;
        WriteRule rule;
        rule.pc = write.pc;
        rule.base = write.base;
        rule.group_class = write.group_class;
        rule.shift = write.shift < 24 ? write.shift : 8;
        rule.x_offset = write.x_offset;
        rule.y_offset = write.y_offset;
        m_writes.push_back(rule);
        ClassInfo &info = m_classes[write.group_class];
        info.shift = rule.shift;
        if (rule.x_offset != 0xffff) info.x_offset = rule.x_offset;
        if (rule.y_offset != 0xffff) info.y_offset = rule.y_offset;
        info.valid = true;
    }
    std::sort(m_writes.begin(), m_writes.end(),
              [](const WriteRule &a, const WriteRule &b) { return a.pc < b.pc; });
    m_resets.reserve(table.resets.size());
    for (const SpriteGroupReset &reset : table.resets) {
        ResetRule rule;
        rule.pc = reset.pc;
        rule.base = reset.base;
        rule.offset = reset.offset;
        m_resets.push_back(rule);
    }
    std::sort(m_resets.begin(), m_resets.end(),
              [](const ResetRule &a, const ResetRule &b) { return a.pc < b.pc; });
    for (const ClassInfo &info : m_classes) m_class_count += info.valid;
    m_report.classes = m_class_count;
    m_group_writes = !m_writes.empty();
}

uint32_t SpriteGroupTracker::register_value(unsigned reg) const {
    return unit_register_value(m_machine.cpu, reg);
}

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

uint16_t SpriteGroupTracker::generation(uint32_t address) const {
    const auto it = m_generation.find(address & 0xffffff);
    return it == m_generation.end() ? 0 : it->second;
}

SpriteGroupSlot SpriteGroupTracker::current_root(uint64_t key) const {
    const auto &info = class_info(group_key_class(key));
    const uint32_t base = group_key_base(key);
    const uint32_t root_x = info.x_offset == NO_OFFSET ? NO_ROOT : (base + info.x_offset) & 0xffffff;
    const uint32_t root_y = info.y_offset == NO_OFFSET ? NO_ROOT : (base + info.y_offset) & 0xffffff;
    if (generation(root_x) != uint16_t(key >> 16) || generation(root_y) != uint16_t(key))
        return {};
    const auto memory = ram();
    const auto x = memory.read_ram16(root_x);
    const auto y = memory.read_ram16(root_y);
    if (!x && !y) return {};
    return {key, int32_t(x.value_or(0)) << info.shift, int32_t(y.value_or(0)) << info.shift};
}

const SpriteGroupTracker::WriteRule *SpriteGroupTracker::write_rule(uint32_t pc) const {
    const auto found = std::lower_bound(m_writes.begin(), m_writes.end(), pc,
                                        [](const WriteRule &rule, uint32_t value) {
                                            return rule.pc < value;
                                        });
    return found == m_writes.end() || found->pc != pc ? nullptr : &*found;
}

std::array<uint32_t, 2> SpriteGroupTracker::roots(uint32_t base, const WriteRule &rule) const {
    const ClassInfo &info = m_classes[rule.group_class];
    const uint16_t x_offset = info.x_offset != NO_OFFSET ? info.x_offset : rule.x_offset;
    const uint16_t y_offset = info.y_offset != NO_OFFSET ? info.y_offset : rule.y_offset;
    return {x_offset != NO_OFFSET ? (base + x_offset) & 0xffffff : NO_ROOT,
            y_offset != NO_OFFSET ? (base + y_offset) & 0xffffff : NO_ROOT};
}

void SpriteGroupTracker::note_ram_write(uint32_t address, unsigned width) {
    if (!m_resets.empty()) {
        const uint32_t pc = m_machine.cpu.pc;
        auto it = std::lower_bound(m_resets.begin(), m_resets.end(), pc,
                                   [](const ResetRule &rule, uint32_t value) {
                                       return rule.pc < value;
                                   });
        const uint64_t frame = m_machine.frame;
        for (; it != m_resets.end() && it->pc == pc; ++it) {
            const uint32_t target = (register_value(it->base) + uint32_t(it->offset)) & 0xffffff;
            if (target - address >= width) continue;
            const auto found = m_last_reset.try_emplace(target, ResetState{UINT64_MAX, 0});
            ResetState &last = found.first->second;
            if (!found.second && last.frame == frame) continue;
            const bool periodic =
                !found.second && last.frame != UINT64_MAX && last.frame + 1 == frame;
            if (!periodic) {
                uint16_t &gen = m_generation[target];
                gen = uint16_t(gen + 1);
                ++m_report.generation_bumps;
                ++m_bump_pc[pc];
            } else {
                ++m_report.periodic_resets;
            }
            last.frame = frame;
            last.pc = pc;
        }
    }
}

std::optional<SpriteRamWrite> SpriteGroupTracker::note_write(uint32_t address, unsigned width) {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE < RAM_END - RAM_BASE) {
        note_ram_write(a, width);
        return std::nullopt;
    }
    const uint32_t start = a - GRAPHICS_BASE;
    if (start >= SPRITE_RAM_SIZE) return std::nullopt;
    const SpriteGroupSlot slot_value = resolve_write(m_machine.cpu, ram());
    ++m_report.sprite_writes;
    if (slot_value.key) {
        ++m_report.grouped_writes;
        ++m_report.class_writes[group_key_class(slot_value.key)];
    } else {
        if (!m_report.unresolved_writes) m_report.first_unresolved_pc = m_machine.cpu.pc;
        ++m_report.unresolved_writes;
    }
    for (unsigned i = 0; i < width; ++i) {
        const uint32_t offset = start + i;
        if (offset >= SPRITE_RAM_SIZE) break;
        if (!sprite_slot_listed(offset)) continue;
        const uint16_t slot = sprite_slot_for(offset);
        m_slot[slot] = m_slot[slot].after_write(offset & 15, slot_value);
    }
    return SpriteRamWrite{start, width};
}

void SpriteGroupTracker::count_ambiguity() {
    if (!m_group_writes) return;
    m_origin_seen.clear();
    m_ambiguous.clear();
    for (const SpriteGroupSlot &slot : m_slot) {
        if (!slot.key) continue;
        const uint64_t packed = uint64_t(uint32_t(slot.x)) << 32 | uint32_t(slot.y);
        const auto [it, fresh] = m_origin_seen.try_emplace(slot.key, packed);
        if (!fresh && it->second != packed) m_ambiguous.insert(slot.key);
    }
    uint64_t slots = 0;
    for (const SpriteGroupSlot &slot : m_slot)
        if (slot.key && m_ambiguous.contains(slot.key)) ++slots;
    m_report.ambiguous_keys += m_ambiguous.size();
    m_report.ambiguous_slots += slots;
    if (!m_ambiguous.empty()) ++m_report.ambiguous_key_frames;
    m_report.max_ambiguous_keys_frame =
        std::max<uint64_t>(m_report.max_ambiguous_keys_frame, m_ambiguous.size());
    m_report.max_ambiguous_slots_frame =
        std::max<uint64_t>(m_report.max_ambiguous_slots_frame, slots);
}

void SpriteGroupTracker::reset() {
    m_generation.clear();
    m_last_reset.clear();
    m_slot.fill({});
}

SpriteGroupTracker::Report SpriteGroupTracker::report() const {
    Report result = m_report;
    result.top_bump_pcs.clear();
    for (const auto &[pc, count] : m_bump_pc) result.top_bump_pcs.push_back({pc, count});
    std::sort(result.top_bump_pcs.begin(), result.top_bump_pcs.end(),
              [](const Report::PcCount &a, const Report::PcCount &b) { return a.count > b.count; });
    if (result.top_bump_pcs.size() > 8) result.top_bump_pcs.resize(8);
    return result;
}

}
