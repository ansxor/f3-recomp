#pragma once
// Sprite-group write tracking (render-only; never serialized, never part of Machine state).
//
// A sprite group is the set of sprite-RAM entries whose position depends on the same root
// fields of one object/queue record. The recompiler emits, per game, a table of sprite-RAM
// writers (f3rt/sprite_group.hpp, generated `sprite_groups.hpp`): for each store PC it names
// the register holding the record, the X/Y root-field offsets and a class id. At runtime:
//   * note_write() looks the writer PC up, reads the root fields from work RAM and derives a
//     stable group key (record address, class, per-root generation) and the group origin
//     (sign-extended root << shift, 24.8 scanout units);
//   * the root fields' generations bump when a reset write (SpriteGroupReset, a value not
//     derived from the previous one: spawn/teleport) hits them, so a replaced object never
//     reuses the old key. A birth is an event, not a routine: a reset that also ran for the
//     same root in the immediately preceding frame is a per-frame recomputation/clamp, so it
//     does not bump (the first reset of a frame bumps at most once per root), while a reset
//     after a frame without one still starts a new generation;
//   * every resolved (or unresolved) sprite-RAM store is reported to the write listeners
//     (sprites/replay.hpp) so they never read this state directly;
//   * the decoder gives block members the head's group (the hardware block carries its
//     position in the head entry only).
// Anything unresolved leaves the slot's group 0: the renderer then snaps instead of guessing.
#include "f3rt/cpu_abi.h"
#include "f3rt/sprite_group.hpp"
#include "sprites/presentation.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace f3rt {
class Machine;

// Base register of an emit unit or write rule: 0..7 = d0..d7, 8..15 = a0..a7.
inline uint32_t unit_register_value(const f3_cpu &cpu, unsigned reg) {
    return reg < 8 ? cpu.d[reg] : cpu.a[reg - 8];
}

class SpriteGroupTracker {
public:
    struct ClassInfo {
        uint16_t x_offset = 0xffff, y_offset = 0xffff;
        uint8_t shift = 8;
        bool valid = false;
    };
    struct Report {
        uint64_t sprite_writes = 0;     // sprite-RAM store instructions seen
        uint64_t grouped_writes = 0;    // ... that resolved a group
        uint64_t unresolved_writes = 0; // ... whose PC/base/roots did not
        uint32_t first_unresolved_pc = 0;
        uint64_t classes = 0;                       // write-table classes in use
        std::array<uint64_t, 256> class_writes{};   // resolved sprite writes per class
        uint64_t generation_bumps = 0, periodic_resets = 0;
        // Ambiguity (render-only): one key with several origins in a frame cannot be paired;
        // the renderer snaps it. commandw's multi-part emit site produces this.
        uint64_t ambiguous_key_frames = 0, ambiguous_keys = 0, ambiguous_slots = 0;
        uint64_t max_ambiguous_keys_frame = 0, max_ambiguous_slots_frame = 0;
        struct PcCount {
            uint32_t pc = 0;
            uint64_t count = 0;
        };
        std::vector<PcCount> top_bump_pcs; // by bump count, descending, at most 8
    };

    SpriteGroupTracker(Machine &machine, const SpriteGroupTable &table);
    SpriteGroupTracker(const SpriteGroupTracker &) = delete;
    SpriteGroupTracker &operator=(const SpriteGroupTracker &) = delete;

    // Write listeners are called for every sprite-RAM store (behaviour replay).
    void add_listener(SpriteRamWriteListener *listener) { m_listeners.push_back(listener); }

    // True when the write table declares at least one group writer.
    bool groups_active() const { return m_group_writes; }
    size_t write_rules() const { return m_writes.size(); }
    size_t reset_rules() const { return m_resets.size(); }

    // ---- Read-only view (presentation, behaviour replay) ----
    std::span<const SpriteGroupSlot> slots() const { return m_slot; }
    const SpriteGroupSlot &slot(size_t index) const { return m_slot[index]; }
    const ClassInfo &class_info(unsigned group_class) const {
        return m_classes[group_class & 0xff];
    }
    uint16_t generation(uint32_t address) const;
    bool read_ram16(uint32_t address, int16_t &value) const;

    // Group keys: a bijective packing of (record address, class, per-root generations), so two
    // record addresses or two classes can never share a key. Zero is only the unresolved key.
    //
    // Deliberately NOT in the key: an emission ordinal or the writer PC. commandw's multi-part
    // emit site (0xa0be) emits two origins for one (record, class, generation) in a frame
    // (measured: 49,902 ambiguous sprites / 1500 frames, 46,732 of them from 0xa0be class 1;
    // record address and generation are stable, and different writer PCs explain only 345).
    // Neither dimension is unique, so such a key stays ambiguous and the renderer snaps it
    // (no guessing). The accurate discriminator is the draw-list slot / emission pass (the
    // 0x40885a list or the 0x413c00 + k*0x80 fallback), an open static-trace question.
    static constexpr uint64_t pack_key(uint32_t base, unsigned group_class, uint16_t gen_x,
                                       uint16_t gen_y) {
        return (uint64_t(base & 0xffffff) << 40) | (uint64_t(group_class & 0xff) << 32) |
               (uint64_t(gen_x) << 16) | uint64_t(gen_y);
    }
    static constexpr uint32_t key_base(uint64_t key) { return uint32_t(key >> 40) & 0xffffff; }
    static constexpr unsigned key_class(uint64_t key) { return unsigned((key >> 32) & 0xff); }

    // ---- Machine hooks ----
    // Real-CPU bus writes (Machine::write*).
    void note_write(uint32_t address, unsigned width);
    // Per presentation: counts this frame's ambiguous keys.
    void count_ambiguity();
    // Drops all transient state (machine reset, state load); counters are kept.
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

    void compile_table(const SpriteGroupTable &table);
    uint32_t register_value(unsigned reg) const;
    bool resolve_write(uint32_t pc, uint32_t &base, uint64_t &key, int32_t &origin_x,
                       int32_t &origin_y);
    void note_ram_write(uint32_t address, unsigned width);
    void sprite_write(uint32_t address, unsigned width);

    Machine &m_machine;
    std::vector<SpriteRamWriteListener *> m_listeners;

    std::vector<WriteRule> m_writes; // sorted by pc
    std::vector<ResetRule> m_resets; // sorted by pc
    std::array<ClassInfo, 256> m_classes{};
    uint64_t m_class_count = 0;
    bool m_group_writes = false;
    std::array<SpriteGroupSlot, SPRITE_SLOTS> m_slot{};
    std::unordered_map<uint32_t, uint16_t> m_generation; // root field address -> generation
    struct ResetState {
        uint64_t frame = 0;
        uint32_t pc = 0;
    };
    std::unordered_map<uint32_t, ResetState> m_last_reset; // root field address
    std::unordered_map<uint32_t, uint64_t> m_bump_pc;      // reset PC -> bumps
    std::unordered_map<uint64_t, uint64_t> m_origin_seen;  // key -> packed first origin
    std::unordered_set<uint64_t> m_ambiguous;              // keys with >1 origin this frame

    Report m_report;
};

} // namespace f3rt
