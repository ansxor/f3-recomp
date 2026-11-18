#pragma once
#include "f3rt/emit_unit.hpp"
#include "f3rt/sprite_group.hpp"
#include "sprites/presentation.hpp"
#include <array>
#include <concepts>
#include <optional>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace f3rt {
class Machine;
template <class T>
concept WorkRam = requires(const T &ram, uint32_t address) {
    { ram.read_ram16(address) } -> std::same_as<std::optional<int16_t>>;
};

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
    struct ClassInfo {
        uint16_t x_offset = 0xffff, y_offset = 0xffff;
        uint8_t shift = 8;
        bool valid = false;
    };
    struct Report {
        uint64_t sprite_writes = 0;
        uint64_t grouped_writes = 0;
        uint64_t unresolved_writes = 0;
        uint32_t first_unresolved_pc = 0;
        uint64_t classes = 0;
        std::array<uint64_t, 256> class_writes{};
        uint64_t generation_bumps = 0, periodic_resets = 0;
        uint64_t ambiguous_key_frames = 0, ambiguous_keys = 0, ambiguous_slots = 0;
        uint64_t max_ambiguous_keys_frame = 0, max_ambiguous_slots_frame = 0;
        struct PcCount {
            uint32_t pc = 0;
            uint64_t count = 0;
        };
        std::vector<PcCount> top_bump_pcs;
    };

    SpriteGroupTracker(Machine &machine, const SpriteGroupTable &table);
    SpriteGroupTracker(const SpriteGroupTracker &) = delete;
    SpriteGroupTracker &operator=(const SpriteGroupTracker &) = delete;

    bool groups_active() const { return m_group_writes; }
    size_t write_rules() const { return m_writes.size(); }
    size_t reset_rules() const { return m_resets.size(); }

    std::span<const SpriteGroupSlot> slots() const { return m_slot; }
    const SpriteGroupSlot &slot(size_t index) const { return m_slot[index]; }
    const ClassInfo &class_info(unsigned group_class) const {
        return m_classes[group_class & 0xff];
    }
    uint16_t generation(uint32_t address) const;
    MachineRam ram() const;

    SpriteGroupSlot current_root(uint64_t key) const;
    template <WorkRam Ram>
    SpriteGroupSlot resolve_write(const f3_cpu &cpu, const Ram &ram) const {
        const auto *rule = write_rule(cpu.pc);
        if (!rule) return {};
        const uint32_t base = unit_register_value(cpu, rule->base) & 0xffffff;
        const auto [root_x, root_y] = roots(base, *rule);
        const auto x = ram.read_ram16(root_x);
        const auto y = ram.read_ram16(root_y);
        if (!x && !y) return {};
        return {pack_group_key(base, rule->group_class, generation(root_x), generation(root_y)),
                int32_t(x.value_or(0)) << rule->shift, int32_t(y.value_or(0)) << rule->shift, cpu.pc};
    }

    std::optional<SpriteRamWrite> note_write(uint32_t address, unsigned width);
    void count_ambiguity();
    void reset();

    Report report() const;

private:
    static constexpr uint16_t NO_OFFSET = 0xffff;
    static constexpr uint32_t NO_ROOT = 0xffffffffu;

    struct WriteRule {
        uint32_t pc = 0;
        uint8_t base = 0, group_class = 0, shift = 8;
        uint16_t x_offset = 0xffff, y_offset = 0xffff;
    };
    struct ResetRule {
        uint32_t pc = 0;
        uint8_t base = 0;
        uint16_t offset = 0;
    };

    const WriteRule *write_rule(uint32_t pc) const;
    std::array<uint32_t, 2> roots(uint32_t base, const WriteRule &rule) const;
    void compile_table(const SpriteGroupTable &table);
    uint32_t register_value(unsigned reg) const;
    void note_ram_write(uint32_t address, unsigned width);

    Machine &m_machine;

    std::vector<WriteRule> m_writes;
    std::vector<ResetRule> m_resets;
    std::array<ClassInfo, 256> m_classes{};
    uint64_t m_class_count = 0;
    bool m_group_writes = false;
    std::array<SpriteGroupSlot, SPRITE_SLOTS> m_slot{};
    std::unordered_map<uint32_t, uint16_t> m_generation;
    struct ResetState {
        uint64_t frame = 0;
        uint32_t pc = 0;
    };
    std::unordered_map<uint32_t, ResetState> m_last_reset;
    std::unordered_map<uint32_t, uint64_t> m_bump_pc;
    std::unordered_map<uint64_t, uint64_t> m_origin_seen;
    std::unordered_set<uint64_t> m_ambiguous;

    Report m_report;
};

}
