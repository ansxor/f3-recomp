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
        if (write.group_class == 0) continue; // class 0 is the unresolved key
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

// ---- Group derivation ------------------------------------------------------------------

uint32_t SpriteGroupTracker::register_value(unsigned reg) const {
    return unit_register_value(m_machine.cpu, reg);
}

bool SpriteGroupTracker::read_ram16(uint32_t address, int16_t &value) const {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE >= RAM_END - RAM_BASE) return false;
    const uint32_t offset = a & RAM_MIRROR_MASK;
    if (offset + 2 > m_machine.ram.size()) return false;
    value = int16_t(uint16_t(uint16_t(m_machine.ram[offset]) << 8 | m_machine.ram[offset + 1]));
    return true;
}

uint16_t SpriteGroupTracker::generation(uint32_t address) const {
    const auto it = m_generation.find(address & 0xffffff);
    return it == m_generation.end() ? 0 : it->second;
}

bool SpriteGroupTracker::resolve_write(uint32_t pc, uint32_t &base, uint64_t &key,
                                       int32_t &origin_x, int32_t &origin_y) {
    const auto found = std::lower_bound(m_writes.begin(), m_writes.end(), pc,
                                        [](const WriteRule &rule, uint32_t value) {
                                            return rule.pc < value;
                                        });
    if (found == m_writes.end() || found->pc != pc) return false;
    const WriteRule &rule = *found;
    base = register_value(rule.base) & 0xffffff;
    const ClassInfo &info = m_classes[rule.group_class];
    const uint16_t x_offset = info.x_offset != 0xffff ? info.x_offset : rule.x_offset;
    const uint16_t y_offset = info.y_offset != 0xffff ? info.y_offset : rule.y_offset;
    const uint32_t root_x = x_offset != 0xffff ? (base + x_offset) & 0xffffff : NO_ROOT;
    const uint32_t root_y = y_offset != 0xffff ? (base + y_offset) & 0xffffff : NO_ROOT;
    int16_t value = 0;
    int32_t ox = 0, oy = 0;
    bool resolved = false;
    if (root_x != NO_ROOT && read_ram16(root_x, value)) {
        ox = int32_t(value) << rule.shift;
        resolved = true;
    }
    if (root_y != NO_ROOT && read_ram16(root_y, value)) {
        oy = int32_t(value) << rule.shift;
        resolved = true;
    }
    if (!resolved) return false;
    origin_x = ox;
    origin_y = oy;
    key = pack_key(base, rule.group_class, root_x != NO_ROOT ? generation(root_x) : uint16_t(0),
                   root_y != NO_ROOT ? generation(root_y) : uint16_t(0));
    return true;
}

void SpriteGroupTracker::note_write(uint32_t address, unsigned width) {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE < RAM_END - RAM_BASE) {
        note_ram_write(a, width);
        return;
    }
    if (a - GRAPHICS_BASE < SPRITE_RAM_SIZE) sprite_write(a, width);
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
            if (!found.second && last.frame == frame) continue; // once per root per frame
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

void SpriteGroupTracker::sprite_write(uint32_t address, unsigned width) {
    const uint32_t start = address - GRAPHICS_BASE;
    if (start >= SPRITE_RAM_SIZE) return;
    uint32_t base = 0;
    uint64_t key = 0;
    int32_t origin_x = 0, origin_y = 0;
    ++m_report.sprite_writes;
    if (resolve_write(m_machine.cpu.pc, base, key, origin_x, origin_y)) {
        ++m_report.grouped_writes;
        ++m_report.class_writes[key_class(key)];
    } else {
        key = 0;
        if (!m_report.unresolved_writes) m_report.first_unresolved_pc = m_machine.cpu.pc;
        ++m_report.unresolved_writes;
    }
    const SpriteGroupSlot slot_value{key, origin_x, origin_y, m_machine.cpu.pc};
    for (unsigned i = 0; i < width; ++i) {
        const uint32_t offset = start + i;
        if (offset >= SPRITE_RAM_SIZE) break;
        if (!sprite_slot_listed(offset)) continue;
        const uint16_t slot = sprite_slot_for(offset);
        // Only the X/Y words (bytes 4..7 of an entry) set or invalidate the position group; a
        // tile/zoom/attribute store leaves the entry's group and origin alone.
        const uint32_t word_offset = offset & 15;
        if (key) {
            m_slot[slot] = slot_value;
        } else if (word_offset >= 4 && word_offset < 8) {
            // A position write the table cannot attribute: snap, never guess.
            m_slot[slot] = {};
        }
    }
    const SpriteRamWrite write{start, width};
    for (SpriteRamWriteListener *listener : m_listeners) listener->sprite_group_write(write);
}

// ---- Per-presentation and per-frame bookkeeping ----------------------------------------

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
    // m_bump_pc is a counter: keep it across a machine reset.
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

} // namespace f3rt
